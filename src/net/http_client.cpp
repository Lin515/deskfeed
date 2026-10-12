#include "net/http_client.h"

#include "core/utf8.h"
#include "net/url.h"
#include "platform/win32_util.h"

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// 一次取数的整次墙钟取消。
//
// 为什么必须是异步 WinHTTP：同步句柄的四段超时（解析/连接/发送/接收）各自独立，且
// WinHttpSetTimeouts 只在请求发出前可靠生效。所以“每次阻塞调用前按剩余预算重设超时”
// 既约束不了已在途的响应头等待，也约束不了逐块滴流的正文——慢头部 + 慢滴流可以让一次
// 取数花掉好几个预算（实测：预算 800/1200/3000 ms 都在约 4 s 的固定天花板返回）。
//
// 异步模型下才有一条可证明的边界，而且取消是文档化行为（Concurrency in WinHTTP：
// 异步操作尚未完成时，应用可以通过关闭相关句柄来请求取消）。线程模型固定为：
// **驱动线程**（调用 http_get 的那一条）独占所有 WinHTTP 句柄操作——发起操作、等到完成
// 通知、再发起下一个；完成回调只做“记下状态 + 唤醒”，不碰任何 WinHTTP 调用。取消时驱动
// 线程没有卡在 WinHTTP 里，可以合法地自己关掉请求句柄，不存在跨线程关句柄的竞态。
//
// 生命周期是这份实现里最容易写错的地方，规则有三条：
//
// 1. **每个句柄持一份强引用，只有它自己的 HANDLE_CLOSING 才还**。文档保证 HANDLE_CLOSING
//    是某个句柄的最后一次回调（“There will be no more callbacks for this handle”）。所以
//    只要有句柄还没收到它，上下文就一定活着；驱动线程最后还掉自己那一份时，即使还有句柄
//    在收尾也只是“还引用”，不会把还活着的对象删掉。收尾时那次等待只是礼貌等待，
//    正确性不依赖它超不超时。
// 2. **异步读的落地缓冲区跟着上下文活**（read_scratch），不能放在会被提前销毁的局部
//    std::string 上：取消时可能还有一次 WinHttpReadData 在途，而它写入的正是那块内存。
//    只有读完成之后，驱动线程才把读到的字节拷进组装用的 body。
// 3. **异步操作的 OUT 参数一律传 NULL**（官方安全须知：注册了回调就等于可能有异步操作，
//    lpdwDataAvailable / lpdwBytesRead 这类指针可能在线程结束后被写，必须传 NULL）。
//    字节数从完成通知里取——DATA_AVAILABLE 的 lpvStatusInformation 是 DWORD 字节数，
//    READ_COMPLETE 的 lpvStatusInformation 是**我们交出去的缓冲区**、字节数在
//    dwStatusInformationLength 里。

namespace deskfeed {
namespace {

constexpr long long kDefaultTimeoutMs = 8000;
constexpr long long kAbortPollMs = 50;     // 取消标志的轮询间隔（换源/退出要够快）
constexpr long long kCloseWaitMs = 1500;   // 收句柄的礼貌等待上限（不是正确性依赖）

long long now_ms() { return static_cast<long long>(::GetTickCount64()); }

std::string to_utf8_safe(std::wstring const& text) { return utf8_from_wide(text); }

int to_timeout_ms(long long remaining) {
    if (remaining <= 0) {
        return 1;
    }
    if (remaining > INT_MAX) {
        return INT_MAX;
    }
    return static_cast<int>(remaining);
}

bool is_redirect_status(DWORD status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

HttpFailure classify(DWORD code, HttpFailure fallback) {
    switch (code) {
        case ERROR_WINHTTP_TIMEOUT:
            return HttpFailure::Timeout;
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        case ERROR_WINHTTP_CANNOT_CONNECT:
        case ERROR_WINHTTP_CONNECTION_ERROR:
        case ERROR_WINHTTP_SECURE_FAILURE:
        case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
        case ERROR_WINHTTP_INCORRECT_HANDLE_STATE:
            return HttpFailure::Connect;
        default:
            return fallback;
    }
}

HttpResult fail(HttpFailure failure, std::string error, long status = 0) {
    HttpResult result;
    result.ok = false;
    result.status = status;
    result.failure = failure;
    result.error = std::move(error);
    return result;
}

bool query_location(HINTERNET request, std::wstring& out) {
    DWORD bytes = 0;
    ::WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, nullptr,
                          &bytes, WINHTTP_NO_HEADER_INDEX);
    if (bytes < sizeof(wchar_t)) {
        return false;
    }
    std::wstring buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (::WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                              buffer.data(), &bytes, WINHTTP_NO_HEADER_INDEX) == FALSE) {
        return false;
    }
    buffer.resize(::wcslen(buffer.c_str()));
    out = buffer;
    return !out.empty();
}

// 驱动线程等待的完成通知位。与 WinHTTP 的状态码一一对应，只保留这一层真正要用的几个。
constexpr std::uint32_t kFlagSendComplete = 0x01;
constexpr std::uint32_t kFlagHeaders = 0x02;
constexpr std::uint32_t kFlagDataAvailable = 0x04;
constexpr std::uint32_t kFlagReadComplete = 0x08;

enum class WaitResult { Signaled, Timeout, Aborted, Failed };

// 一次取数的共享上下文。回调线程与驱动线程都在它上面同步。
struct CallContext {
    std::mutex mutex;
    std::condition_variable cv;

    std::uint32_t flags = 0;   // 已到达的完成通知
    bool failed = false;       // 收到 WINHTTP_CALLBACK_STATUS_REQUEST_ERROR
    DWORD failed_api = 0;      // 哪一个异步 API 失败（API_* 常量，只进日志）
    DWORD failed_code = 0;     // 失败的错误码

    DWORD available = 0;       // DATA_AVAILABLE 报的可用字节数
    DWORD bytes_read = 0;      // READ_COMPLETE 报的实际读到的字节数

    // 当前正在等哪一个请求句柄的完成通知。重定向会一跳一跳地关掉旧请求再建新请求，
    // 旧句柄的迟到通知（尤其是取消/收尾时产生的 REQUEST_ERROR）绝不能计进下一跳的
    // 完成标志或失败标志里——完成通知只在 handle 与它相符时才生效。
    HINTERNET watch_handle = nullptr;

    // 异步读的落地缓冲区。它跟着上下文活到最后一个 HANDLE_CLOSING，所以取消时在途的
    // WinHttpReadData 仍然有一块合法的内存可写。读完成之后才把字节拷进组装用的 body。
    std::vector<unsigned char> read_scratch;

    // 已创建、尚未收到 HANDLE_CLOSING 的句柄数。每创建一个句柄就多持一份引用。
    int open_handles = 0;

    std::atomic<long> refs{1};

    void add_ref() { refs.fetch_add(1, std::memory_order_relaxed); }
    void release() {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }
};

void CALLBACK status_callback(HINTERNET handle, DWORD_PTR context, DWORD status, void* info,
                              DWORD info_length) {
    auto* ctx = reinterpret_cast<CallContext*>(context);
    if (ctx == nullptr) {
        return;
    }
    // 回调可能跑在 WinHTTP 自己的线程池线程上，而且同一线程可能被重入，所以状态一律
    // 在锁里改。入口这一份引用保证对象在整个回调期间活着。
    ctx->add_ref();
    bool handle_closed = false;
    {
        std::lock_guard<std::mutex> guard(ctx->mutex);
        // 只有当前这一跳的请求句柄的完成通知才算数；别的句柄（会话/连接/已经关掉的旧请求）
        // 的通知一律不参与等待判定。HANDLE_CLOSING 例外：任何句柄的都要结算引用。
        bool const watched = ctx->watch_handle != nullptr && handle == ctx->watch_handle;
        switch (status) {
            case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
                if (watched) {
                    ctx->flags |= kFlagSendComplete;
                }
                break;
            case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
                if (watched) {
                    ctx->flags |= kFlagHeaders;
                }
                break;
            case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
                if (watched) {
                    ctx->flags |= kFlagDataAvailable;
                    if (info != nullptr && info_length >= sizeof(DWORD)) {
                        ctx->available = *static_cast<DWORD const*>(info);
                    }
                }
                break;
            case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
                if (watched) {
                    ctx->flags |= kFlagReadComplete;
                    // lpvStatusInformation 是我们交给 WinHttpReadData 的缓冲区，
                    // 真正的字节数在 dwStatusInformationLength 里；数据已经写进缓冲区。
                    ctx->bytes_read = info_length;
                }
                break;
            case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
                if (watched && !ctx->failed) {
                    ctx->failed = true;
                    if (info != nullptr && info_length >= sizeof(WINHTTP_ASYNC_RESULT)) {
                        auto const* async_result = static_cast<WINHTTP_ASYNC_RESULT const*>(info);
                        ctx->failed_api = static_cast<DWORD>(async_result->dwResult);
                        ctx->failed_code = async_result->dwError;
                    }
                }
                break;
            case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
                // 这个句柄不会再有回调了：还掉它那一份引用。
                if (ctx->open_handles > 0) {
                    --ctx->open_handles;
                }
                handle_closed = true;
                break;
            default:
                break;
        }
    }
    ctx->cv.notify_all();
    if (handle_closed) {
        // 必须在锁外还：这一份可能就是最后一份，删除会销毁互斥量与条件变量。
        ctx->release();
    }
    ctx->release();
}

bool set_context(HINTERNET handle, CallContext* ctx) {
    // 每个句柄都显式设一次，不依赖“子句柄继承父句柄上下文”的行为。
    // WinHttpSetOption 在异步模式下也是同步生效的。
    DWORD_PTR value = reinterpret_cast<DWORD_PTR>(ctx);
    return ::WinHttpSetOption(handle, WINHTTP_OPTION_CONTEXT_VALUE, &value,
                              static_cast<DWORD>(sizeof(value))) != FALSE;
}

void reset_flags(CallContext& ctx, HINTERNET watch) {
    std::lock_guard<std::mutex> guard(ctx.mutex);
    ctx.flags = 0;
    ctx.failed = false;
    ctx.failed_api = 0;
    ctx.failed_code = 0;
    ctx.available = 0;
    ctx.bytes_read = 0;
    ctx.watch_handle = watch;
}

DWORD read_available(CallContext& ctx) {
    std::lock_guard<std::mutex> guard(ctx.mutex);
    return ctx.available;
}

DWORD read_bytes_read(CallContext& ctx) {
    std::lock_guard<std::mutex> guard(ctx.mutex);
    return ctx.bytes_read;
}

// 为一次异步读准备落地缓冲区，并返回它的地址。缓冲区属于上下文，取消时也不会提前释放。
unsigned char* prepare_read_scratch(CallContext& ctx, DWORD size) {
    std::lock_guard<std::mutex> guard(ctx.mutex);
    ctx.read_scratch.assign(static_cast<std::size_t>(size), 0);
    return ctx.read_scratch.data();
}

// 等某个完成通知，或者到墙钟截止，或者被要求取消。这是整次预算的唯一出处。
WaitResult wait_for(CallContext& ctx, std::uint32_t wanted, long long deadline,
                    std::atomic<bool> const* abort) {
    std::unique_lock<std::mutex> lock(ctx.mutex);
    for (;;) {
        if ((ctx.flags & wanted) != 0) {
            return WaitResult::Signaled;
        }
        if (ctx.failed) {
            return WaitResult::Failed;
        }
        long long const remaining = deadline - now_ms();
        if (remaining <= 0) {
            return WaitResult::Timeout;
        }
        if (abort != nullptr && abort->load()) {
            return WaitResult::Aborted;
        }
        long long const slice = remaining < kAbortPollMs ? remaining : kAbortPollMs;
        ctx.cv.wait_for(lock, std::chrono::milliseconds(slice));
    }
}

// 驱动线程已经决定结果，等所有句柄收到 HANDLE_CLOSING。这只是礼貌等待：等不到也没关系，
// 上下文由每个句柄自己那一份引用持有，最后一次 HANDLE_CLOSING 会把它释放掉。
void wait_for_close(CallContext& ctx) {
    std::unique_lock<std::mutex> lock(ctx.mutex);
    ctx.cv.wait_for(lock, std::chrono::milliseconds(kCloseWaitMs),
                    [&ctx] { return ctx.open_handles <= 0; });
}

}  // namespace

char const* http_failure_name(HttpFailure failure) {
    switch (failure) {
        case HttpFailure::None: return "none";
        case HttpFailure::InvalidUrl: return "invalid-url";
        case HttpFailure::Connect: return "connect";
        case HttpFailure::Timeout: return "timeout";
        case HttpFailure::Send: return "send";
        case HttpFailure::Receive: return "receive";
        case HttpFailure::Redirect: return "redirect";
        case HttpFailure::Status: return "http-status";
        case HttpFailure::TooLarge: return "too-large";
        case HttpFailure::Empty: return "empty";
        case HttpFailure::Aborted: return "aborted";
    }
    return "unknown";
}

HttpResult http_get(std::wstring const& url, HttpLimits const& limits,
                    std::atomic<bool> const* abort) {
    long long const budget = limits.timeout_ms > 0 ? limits.timeout_ms : kDefaultTimeoutMs;
    long long const deadline = now_ms() + budget;

    if (abort != nullptr && abort->load()) {
        return fail(HttpFailure::Aborted, "取数已取消");
    }

    auto* ctx = new CallContext();

    // NO_PROXY：回环地址走代理没有意义，且无网络时 WinHTTP 的自动代理探测会白等很久。
    HINTERNET const session = ::WinHttpOpen(L"DeskFeed/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                                            WINHTTP_FLAG_ASYNC);
    if (session == nullptr) {
        DWORD const code = ::GetLastError();
        ctx->release();
        return fail(HttpFailure::Connect, "WinHttpOpen 失败：" + to_utf8_safe(last_error_text(code)));
    }

    // 回调与上下文都必须挂上，否则完成通知永远不会来、每个操作都会等到截止。
    // 挂之前没有任何操作在途，所以不存在“回调先于上下文到达”的窗口。
    if (::WinHttpSetStatusCallback(session, &status_callback,
                                   WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE |
                                       WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE |
                                       WINHTTP_CALLBACK_FLAG_DATA_AVAILABLE |
                                       WINHTTP_CALLBACK_FLAG_READ_COMPLETE |
                                       WINHTTP_CALLBACK_FLAG_REQUEST_ERROR |
                                       WINHTTP_CALLBACK_FLAG_HANDLES,
                                   0) == WINHTTP_INVALID_STATUS_CALLBACK) {
        DWORD const code = ::GetLastError();
        ::WinHttpCloseHandle(session);
        ctx->release();
        return fail(HttpFailure::Connect,
                    "装不上 WinHTTP 状态回调：" + to_utf8_safe(last_error_text(code)));
    }
    if (!set_context(session, ctx)) {
        DWORD const code = ::GetLastError();
        ::WinHttpCloseHandle(session);
        ctx->release();
        return fail(HttpFailure::Connect,
                    "记不住 WinHTTP 会话上下文：" + to_utf8_safe(last_error_text(code)));
    }

    // 会话句柄的强引用：只有它的 HANDLE_CLOSING 才还这一份。
    {
        std::lock_guard<std::mutex> guard(ctx->mutex);
        ctx->open_handles = 1;
    }
    ctx->add_ref();

    // WinHTTP 自己的四段超时只是兜底：真正的整次截止由驱动线程守，取消靠关请求句柄。
    ::WinHttpSetTimeouts(session, to_timeout_ms(budget), to_timeout_ms(budget),
                         to_timeout_ms(budget), to_timeout_ms(budget));

    auto budget_error = [budget] {
        return fail(HttpFailure::Timeout,
                    "超过 " + std::to_string(budget) + " ms 整次取数预算");
    };

    HttpResult result;
    std::wstring current = url;
    int redirects = 0;

    HINTERNET connection = nullptr;
    HINTERNET request = nullptr;

    // 新建的句柄：先记下“它那一份引用”，再挂上下文。挂上下文之前没有任何操作在途，
    // 所以不会有回调抢在前面。挂不上就自己把这一份还掉，避免引用泄漏。
    auto adopt_handle = [&](HINTERNET handle) -> bool {
        {
            std::lock_guard<std::mutex> guard(ctx->mutex);
            ++ctx->open_handles;
        }
        ctx->add_ref();
        if (set_context(handle, ctx)) {
            return true;
        }
        {
            std::lock_guard<std::mutex> guard(ctx->mutex);
            if (ctx->open_handles > 0) {
                --ctx->open_handles;
            }
        }
        ctx->release();
        return false;
    };

    // 发起一个操作并等它的完成通知。issue 返回 FALSE 时只有 ERROR_IO_PENDING 是正常的：
    // 异步模式下所有请求操作的完成都靠回调。
    auto finish_wait = [&](WaitResult waited, HttpFailure fallback,
                           std::string const& what) -> bool {
        switch (waited) {
            case WaitResult::Signaled:
                return true;
            case WaitResult::Timeout:
                result = budget_error();
                return false;
            case WaitResult::Aborted:
                result = fail(HttpFailure::Aborted, "取数已取消");
                return false;
            case WaitResult::Failed: {
                DWORD code = 0;
                {
                    std::lock_guard<std::mutex> guard(ctx->mutex);
                    code = ctx->failed_code;
                }
                if (code == ERROR_WINHTTP_TIMEOUT) {
                    result = budget_error();
                } else {
                    result = fail(classify(code, fallback),
                                  what + "失败：" + to_utf8_safe(last_error_text(code)));
                }
                return false;
            }
        }
        return false;
    };

    auto close_hop = [&] {
        // 顺序固定：先请求、再连接；会话留到整次取数结束。每个句柄只关一次。
        if (request != nullptr) {
            ::WinHttpCloseHandle(request);
            request = nullptr;
        }
        if (connection != nullptr) {
            ::WinHttpCloseHandle(connection);
            connection = nullptr;
        }
    };

    for (;;) {
        if (abort != nullptr && abort->load()) {
            result = fail(HttpFailure::Aborted, "取数已取消");
            break;
        }
        if (deadline - now_ms() <= 0) {
            result = budget_error();
            break;
        }

        UrlParts parts;
        std::wstring url_error;
        if (!url_crack(current, parts, url_error)) {
            result = fail(HttpFailure::InvalidUrl, to_utf8_safe(url_error));
            break;
        }

        connection = ::WinHttpConnect(session, parts.host.c_str(), parts.port, 0);
        if (connection == nullptr) {
            DWORD const code = ::GetLastError();
            result = fail(classify(code, HttpFailure::Connect),
                          "连接失败：" + to_utf8_safe(last_error_text(code)));
            break;
        }
        if (!adopt_handle(connection)) {
            result = fail(HttpFailure::Connect, "记不住 WinHTTP 连接上下文");
            break;
        }

        request = ::WinHttpOpenRequest(connection, L"GET", parts.path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       parts.secure ? WINHTTP_FLAG_SECURE : 0);
        if (request == nullptr) {
            DWORD const code = ::GetLastError();
            result = fail(classify(code, HttpFailure::Connect),
                          "建不了请求：" + to_utf8_safe(last_error_text(code)));
            break;
        }
        if (!adopt_handle(request)) {
            result = fail(HttpFailure::Connect, "记不住 WinHTTP 请求上下文");
            break;
        }

        // 重定向由这一层自己跟：策略设为“从不自动跟”，这样每一跳的协议都能自己检查。
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        ::WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));

        // 1) 发送请求
        reset_flags(*ctx, request);
        if (::WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                                 0, 0, 0) == FALSE) {
            DWORD const code = ::GetLastError();
            if (code != ERROR_IO_PENDING) {
                result = fail(classify(code, HttpFailure::Send),
                              "请求发送失败：" + to_utf8_safe(last_error_text(code)));
                break;
            }
        }
        if (!finish_wait(wait_for(*ctx, kFlagSendComplete, deadline, abort), HttpFailure::Send,
                         "请求发送")) {
            break;
        }

        // 2) 接收响应头
        reset_flags(*ctx, request);
        if (::WinHttpReceiveResponse(request, nullptr) == FALSE) {
            DWORD const code = ::GetLastError();
            if (code != ERROR_IO_PENDING) {
                result = fail(classify(code, HttpFailure::Receive),
                              "没有收到响应：" + to_utf8_safe(last_error_text(code)));
                break;
            }
        }
        if (!finish_wait(wait_for(*ctx, kFlagHeaders, deadline, abort), HttpFailure::Receive,
                         "接收响应")) {
            break;
        }

        DWORD status = 0;
        DWORD status_size = sizeof(status);
        if (::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                  WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                                  WINHTTP_NO_HEADER_INDEX) == FALSE) {
            DWORD const code = ::GetLastError();
            result = fail(classify(code, HttpFailure::Receive),
                          "读不到响应状态码：" + to_utf8_safe(last_error_text(code)));
            break;
        }

        if (is_redirect_status(status)) {
            std::wstring location;
            if (!query_location(request, location)) {
                result = fail(HttpFailure::Status,
                              "上游返回 " + std::to_string(status) + " 但没有 Location",
                              static_cast<long>(status));
                break;
            }
            if (redirects >= limits.max_redirects) {
                result = fail(HttpFailure::Redirect,
                              "重定向超过 " + std::to_string(limits.max_redirects) + " 跳",
                              static_cast<long>(status));
                break;
            }
            std::wstring next;
            std::wstring resolve_error;
            if (!url_resolve(current, location, next, resolve_error)) {
                result = fail(HttpFailure::Redirect, "无法解析重定向目标：" + to_utf8_safe(resolve_error),
                              static_cast<long>(status));
                break;
            }
            UrlParts next_parts;
            if (!url_crack(next, next_parts, resolve_error)) {
                result = fail(HttpFailure::Redirect,
                              "重定向目标不合法：" + to_utf8_safe(resolve_error),
                              static_cast<long>(status));
                break;
            }
            if (parts.secure && !next_parts.secure) {
                result = fail(HttpFailure::Redirect, "禁止从 HTTPS 降级到 HTTP",
                              static_cast<long>(status));
                break;
            }
            ++redirects;
            current = std::move(next);
            close_hop();  // 这一跳的句柄立刻收掉，下一跳重新建连接
            continue;
        }

        result.status = static_cast<long>(status);
        if (status != 200) {
            // 204 / 304 / 3xx（没有 Location）/ 4xx / 5xx 都不是快照。
            result = fail(HttpFailure::Status, "上游状态码 " + std::to_string(status),
                          static_cast<long>(status));
            break;
        }

        // 3) 逐块读正文（边读边判上限）。读到的字节先落在上下文持有的 read_scratch 上，
        //    读完成之后才拷进 body：取消时在途的那次读仍然有合法的内存可写。
        std::string body;
        bool body_done = false;
        bool body_failed = false;
        for (;;) {
            reset_flags(*ctx, request);
            // 异步操作：OUT 参数必须是 NULL（官方安全须知），字节数从完成通知里取。
            if (::WinHttpQueryDataAvailable(request, nullptr) == FALSE) {
                DWORD const code = ::GetLastError();
                if (code != ERROR_IO_PENDING) {
                    result = fail(classify(code, HttpFailure::Receive),
                                  "读取响应长度失败：" + to_utf8_safe(last_error_text(code)));
                    body_failed = true;
                    break;
                }
            }
            if (!finish_wait(wait_for(*ctx, kFlagDataAvailable, deadline, abort),
                             HttpFailure::Receive, "读取响应长度")) {
                body_failed = true;
                break;
            }

            DWORD const available = read_available(*ctx);
            if (available == 0) {
                body_done = true;
                break;
            }
            if (body.size() + available > limits.max_body_bytes) {
                result = fail(HttpFailure::TooLarge,
                              "正文超过 " + std::to_string(limits.max_body_bytes) + " 字节");
                body_failed = true;
                break;
            }

            unsigned char* const scratch = prepare_read_scratch(*ctx, available);
            reset_flags(*ctx, request);
            if (::WinHttpReadData(request, scratch, available, nullptr) == FALSE) {
                DWORD const code = ::GetLastError();
                if (code != ERROR_IO_PENDING) {
                    result = fail(classify(code, HttpFailure::Receive),
                                  "读取响应失败：" + to_utf8_safe(last_error_text(code)));
                    body_failed = true;
                    break;
                }
            }
            if (!finish_wait(wait_for(*ctx, kFlagReadComplete, deadline, abort),
                             HttpFailure::Receive, "读取响应")) {
                body_failed = true;
                break;
            }

            DWORD const read = read_bytes_read(*ctx);
            if (read > 0) {
                body.append(reinterpret_cast<char const*>(scratch),
                            static_cast<std::size_t>(read));
            }
            if (read == 0) {
                body_done = true;
                break;
            }
        }
        if (body_failed) {
            break;
        }
        if (!body_done) {
            result = budget_error();
            break;
        }

        if (body.empty()) {
            result = fail(HttpFailure::Empty, "响应正文是空的");
            break;
        }
        result.ok = true;
        result.failure = HttpFailure::None;
        result.body = std::move(body);
        break;
    }

    // 收尾：把这一跳还没收的句柄按固定顺序关掉。取消/超时就是在这里请求取消
    // （异步操作未完成时关闭相关句柄是文档化的取消方式，而驱动线程此刻没有在 WinHTTP 里）。
    close_hop();
    ::WinHttpCloseHandle(session);
    wait_for_close(*ctx);
    ctx->release();  // 驱动线程那一份；还有句柄未收完时对象继续活着
    return result;
}

}  // namespace deskfeed
