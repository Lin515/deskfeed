#include "master/master.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "core/health.h"
#include "core/schedule.h"
#include "core/snapshot.h"
#include "core/utf8.h"
#include "display/frame.h"
#include "display/mailbox.h"
#include "ipc/binding.h"
#include "ipc/command.h"
#include "ipc/control_channel.h"
#include "ipc/settings.h"
#include "master/taskbar_supervisor.h"
#include "net/http_client.h"
#include "net/url.h"
#include "platform/console.h"
#include "platform/log.h"
#include "platform/paths.h"
#include "platform/user_scope.h"
#include "platform/win32_util.h"
#include "widget/preferences.h"
#include "widget/window.h"

namespace deskfeed {
namespace {

constexpr UINT kMsgFetchDone = WM_APP + 1;   // 唤醒消息，不带载荷（结果在有界队列里）
constexpr UINT kMsgBindingExit = WM_APP + 2; // wParam = 绑定代次
constexpr UINT kMsgQuit = WM_APP + 3;
// 任务栏组件（载荷 DLL）提出的动作请求。控制窗口过程只**投递**这两条消息就返回，
// 真正的取数/显窗在主循环里做：窗口过程在 SendMessageTimeoutW 的调用方线程上，
// 在那里做网络或磁盘工作会把别人的 UI 线程按 3 秒的预算挂住。
constexpr UINT kMsgRefreshRequested = WM_APP + 4;
constexpr UINT kMsgShowRequested = WM_APP + 5;
// 心跳：主循环在嵌套模态循环（右键菜单的 TrackPopupMenu）期间跑不到，靠这个定时器把状态机
// 继续推下去。1 s 只是“模态期间”的兜底粒度；没有模态时主循环自己按精确的到期时刻醒来，
// 心跳只做一次空转（revision 没变就不发帧，代价可以忽略）。
constexpr UINT kHeartbeatMs = 1000;
constexpr unsigned kControlSendTimeoutMs = 3000;
constexpr unsigned kShutdownJoinMs = 2000;
// 悬浮窗自己的窗口类名基础部分（与隔离实例的 scope 一起由 scoped_name 展开，见
// platform/paths.h）。控制窗口用 DeskFeedControl，这里的窗口只有主进程自己用。
constexpr wchar_t kWidgetWindowClassBase[] = L"DeskFeedWidget";
// 同时在跑的“有效”取数上限。取消现在是真的（HTTP 层到点或被要求就关掉在途请求句柄），
// 所以一个被放弃的作业只占几十毫秒；它不再挡着新源的首取——名额按 active 计数，
// 一取消就释放。kMaxFetchJobs 是作业记录的硬上限，只用来兜住极端情况下的堆积。
constexpr std::size_t kMaxLiveFetches = 2;
constexpr std::size_t kMaxFetchJobs = 8;

// 测试注入点最多同时延迟这么多条退出消息（见 Master 的 defer_quits_）：到顶就当场消费，
// 队列不会无界增长。
constexpr std::size_t kMaxDeferredQuits = 8;
// 延迟注入点允许的最大毫秒数（只对 scope 非空 + headless 生效）。
constexpr long long kMaxDeferQuitMs = 10000;

long long monotonic_ms() { return static_cast<long long>(::GetTickCount64()); }

// 健康/到期用的是快照里的绝对时间（Unix 毫秒），所以这一层必须有真实的墙钟；
// 调度与超时用单调时钟（monotonic_ms），两者不能混用，也不能互相换算。
long long wall_ms() {
    FILETIME file_time{};
    ::GetSystemTimeAsFileTime(&file_time);
    ULARGE_INTEGER value{};
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    return static_cast<long long>(value.QuadPart / 10000ULL) - 11644473600000LL;
}

std::wstring redact(std::wstring const& url) {
    bool removed = false;
    std::wstring const out = url_redact(url, &removed);
    return out;
}

std::string json_escape(std::string const& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char const byte : text) {
        switch (byte) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (byte < 0x20) {
                    char buffer[8]{};
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", byte);
                    out += buffer;
                } else {
                    out.push_back(static_cast<char>(byte));
                }
        }
    }
    return out;
}

std::string json_string(std::string const& text) { return "\"" + json_escape(text) + "\""; }

std::string json_wstring(std::wstring const& text) { return json_string(utf8_from_wide(text)); }

// 隔离实例专用开关用的环境变量读取。只给测试失败注入点用；生产实例根本不查它。
std::wstring read_test_environment(std::wstring const& name) {
    wchar_t buffer[64]{};
    DWORD const chars =
        ::GetEnvironmentVariableW(name.c_str(), buffer, static_cast<DWORD>(std::size(buffer)));
    if (chars == 0 || chars >= std::size(buffer)) {
        return std::wstring();
    }
    return std::wstring(buffer, chars);
}

// `[taskbar] component` 的读取：只认 true / false（大小写不敏感），缺省 true，写坏的值按缺省
// 处理。口径与 widget/preferences.cpp 的 read_bool 一致，免得同一个键出现两种解释。
// 这一份是**用户的持久意图**；它和“本次运行有没有能力提供”是两件事（见 Master 的成员说明）。
bool read_taskbar_component_intent(SettingsFile const* settings) {
    if (settings == nullptr) {
        return true;
    }
    std::wstring text;
    if (!settings->get(L"taskbar", L"component", text)) {
        return true;
    }
    std::wstring body;
    for (wchar_t const ch : text) {
        if (ch != L' ' && ch != L'\t') {
            body.push_back(ch);
        }
    }
    if (_wcsicmp(body.c_str(), L"true") == 0) {
        return true;
    }
    if (_wcsicmp(body.c_str(), L"false") == 0) {
        return false;
    }
    return true;
}

struct FetchOutcome {
    std::uint64_t job_id = 0;
    std::uint64_t generation = 0;
    HttpResult http;
    ValidationResult validation;
    Snapshot snapshot;
};

// 取数结果从工作线程交回主进程的唯一通道：**有界队列** + 一条不带指针的唤醒消息。
//
// 上限就是作业记录的硬上限：同时在跑的作业不可能比它多，超了只可能是重复投递。
constexpr std::size_t kOutcomeQueueMax = kMaxFetchJobs;
//
// 为什么不是“PostMessage 一个 new 出来的指针”：主进程的消息可能被**嵌套模态循环**派发
// （右键菜单的 TrackPopupMenu 就在主进程线程上开一个模态循环）。带指针的 WM_APP 一旦落进
// 那个循环，只有控制窗口过程认识它才行；落到 DefWindowProc 上指针就被静默丢掉——结果丢失，
// 还漏一块内存。所以这里把结果放进队列，唤醒消息的 wParam/lParam 一个字节的载荷都不带：
// 谁派发这条消息都只会触发“去队列里取”，取不到东西也不会丢任何指针。
//
// **生命周期**：这个对象用 shared_ptr 共享所有权——主进程持一份，每个工作线程在它整个运行
// 期间也持一份。主进程收尾时只是停止接受新结果并丢掉自己那一份；只要还有线程没真正 return，
// 队列（连同它那把锁）就还活着。绝不允许出现“主进程析构了队列，而某个迟到的线程还在往里
// push”的那种 use-after-free：收尾那 2 秒的等待是兜底，不是生命周期保证。
class OutcomeQueue {
public:
    static std::shared_ptr<OutcomeQueue> create() {
        // 不能用 make_shared：构造函数/析构函数是私有的，要的就是“只能从这里建”。
        return std::shared_ptr<OutcomeQueue>(new OutcomeQueue());
    }

    ~OutcomeQueue() { ::DeleteCriticalSection(&lock_); }

    OutcomeQueue(OutcomeQueue const&) = delete;
    OutcomeQueue& operator=(OutcomeQueue const&) = delete;

    // 主进程不再接受新结果。之后工作线程入队一律被拒（它们自己会按“入队失败”处理）。
    void stop_accepting() {
        ::EnterCriticalSection(&lock_);
        accepting_ = false;
        items_.clear();
        ::LeaveCriticalSection(&lock_);
    }

    // 入队。满员或已经停止接受时返回 false（调用方据此把这次取数当失败收尾）。
    // RAII 持有锁：即使 vector 的扩容抛异常，锁也一定会释放。
    bool push(std::unique_ptr<FetchOutcome> outcome) {
        CriticalSectionGuard const guard(lock_);
        if (!accepting_ || items_.size() >= kOutcomeQueueMax) {
            return false;
        }
        items_.push_back(std::move(outcome));
        return true;
    }

    // 取走一条。空队列返回 nullptr。
    std::unique_ptr<FetchOutcome> pop() {
        CriticalSectionGuard const guard(lock_);
        if (items_.empty()) {
            return nullptr;
        }
        std::unique_ptr<FetchOutcome> outcome = std::move(items_.front());
        items_.erase(items_.begin());
        return outcome;
    }

private:
    OutcomeQueue() {
        // 顺序是有意的：**先 reserve，再建锁**。
        //
        // vector::reserve 失败会抛 std::bad_alloc，而构造函数一旦抛出，析构函数**不会**被调用
        // （对象从未构造完成），于是先建好的 CRITICAL_SECTION 就没人 DeleteCriticalSection 了。
        // 反过来先 reserve：分配失败时还没有任何需要归还的资源，构造失败是干净的。
        // 这里不存在“先拿锁再分配”的逆序问题——reserve 不碰锁，锁也只在成员函数里用。
        items_.reserve(kOutcomeQueueMax);
        // 预分配之后 push 不会再扩容（满员由 kOutcomeQueueMax 提前挡住），push 里因此不分配。
        ::InitializeCriticalSection(&lock_);
    }

    // 一把锁的 RAII 帮手：这个文件里只有这一处用到锁，不值得单开一个头。
    class CriticalSectionGuard {
    public:
        explicit CriticalSectionGuard(CRITICAL_SECTION& lock) : lock_(lock) {
            ::EnterCriticalSection(&lock_);
        }
        ~CriticalSectionGuard() { ::LeaveCriticalSection(&lock_); }
        CriticalSectionGuard(CriticalSectionGuard const&) = delete;
        CriticalSectionGuard& operator=(CriticalSectionGuard const&) = delete;

    private:
        CRITICAL_SECTION& lock_;
    };

    CRITICAL_SECTION lock_{};
    bool accepting_ = true;
    std::vector<std::unique_ptr<FetchOutcome>> items_;
};

struct FetchJobContext {
    std::uint64_t job_id = 0;
    std::uint64_t generation = 0;
    std::wstring url;
    HttpLimits limits;
    std::shared_ptr<std::atomic<bool>> abort;
    HWND notify = nullptr;
    // 这个线程**自己持有一份**队列所有权：只要它还没 return，队列与那把锁就不会被销毁。
    std::shared_ptr<OutcomeQueue> queue;
    // 结果没能入队（满员 / 已经停止接受 / 线程内异常）时置起来：主进程据此立刻把这次取数
    // 当失败收尾并释放名额，不用干等墙钟预算。
    std::shared_ptr<std::atomic<bool>> finished_without_result;
    // 测试失败注入点（见 Master::test_refuse_outcome_）：让这一次的入队**必定失败**，
    // 用来覆盖“结果没能交回主进程 ⇒ 立刻按内部失败收尾”这条路径。只在隔离实例里为真。
    bool refuse_outcome = false;
};

// 工作线程的实际工作体。它**不是**异常边界，调用方 `fetch_thread_entry` 才是。
//
// 这里保留一条**诊断用**的内层 catch（把异常降级成一条带 what() 的“内部失败”结果），因为它
// 能给出可读的原因。但它不构成安全边界：`std::string` 拼接与 `what()` 自己也会抛。
void fetch_worker_body(FetchJobContext* context, std::shared_ptr<OutcomeQueue> const& queue) {
    auto outcome = std::make_unique<FetchOutcome>();
    outcome->job_id = context->job_id;
    outcome->generation = context->generation;
    bool failed = false;
    try {
        outcome->http = http_get(context->url, context->limits, context->abort.get());
        if (outcome->http.ok) {
            outcome->validation = snapshot_validate(outcome->http.body, outcome->snapshot);
        }
    } catch (std::exception const& error) {
        outcome->http = HttpResult{};
        outcome->http.ok = false;
        outcome->http.failure = HttpFailure::None;
        outcome->http.error = std::string("取数线程内异常：") + error.what();
        failed = true;
    } catch (...) {
        outcome->http = HttpResult{};
        outcome->http.ok = false;
        outcome->http.failure = HttpFailure::None;
        outcome->http.error = "取数线程内未知异常";
        failed = true;
    }
    if (failed) {
        outcome->http.failure = HttpFailure::Aborted;  // 主进程按“内部失败”收尾，不当成上游错误
    }

    // 入队；队列满了或已经停止接受就把这块结果丢掉（有上限的降级，绝不无界增长），
    // 并置 finished_without_result 让主进程立刻收尾这次取数。
    bool queued = false;
    if (queue && !context->refuse_outcome) {
        queued = queue->push(std::move(outcome));
    }
    if (!queued) {
        log_format(L"fetch: 结果没能入队，丢弃 job %llu 的结果", context->job_id);
        if (context->finished_without_result) {
            context->finished_without_result->store(true);
        }
    }
    ::PostMessageW(context->notify, kMsgFetchDone, 0, 0);  // 只叫醒，不带指针
}

// 工作线程入口：这里是**普通线程的真实出口**，异常一旦跨出去就是 std::terminate（整个主进程
// 直接没）。所以整个函数体都压在一条最外层边界里，包括以前漏在外面的三处：
//   * `std::make_unique<FetchOutcome>()`（分配失败）；
//   * 诊断 catch 里的 `std::string` 拼接与 `what()`（它们自己也会抛）；
//   * `queue->push`，以及入队失败时的 `log_format`。
// 最外层 catch 只做两件**不分配、不记日志、不碰别的对象**的事：置“结果没能入队”标志、投递
// 一条不带指针的唤醒消息。这不是一套 OOM 兜底框架，只保证当前这条 OS 线程有一个安全的出口；
// `context` 这个 unique_ptr 由本函数持有（在边界之外），无论走哪条路都会安全析构。
DWORD WINAPI fetch_thread_entry(void* parameter) {
    std::unique_ptr<FetchJobContext> context(static_cast<FetchJobContext*>(parameter));
    try {
        if (context) {
            std::shared_ptr<OutcomeQueue> const queue = context->queue;
            fetch_worker_body(context.get(), queue);
        }
    } catch (...) {
        // 能走到这里，说明工作体在“结果成功入队”之前就抛了：push 成功之后只剩原子写与一次
        // PostMessageW，两者都不抛。所以置这个标志不会把一条已经交回来的结果误判成丢失。
        if (context) {
            if (context->finished_without_result) {
                context->finished_without_result->store(true);
            }
            if (context->notify != nullptr) {
                ::PostMessageW(context->notify, kMsgFetchDone, 0, 0);
            }
        }
    }
    return 0;
}

FailureKind failure_from_http(HttpFailure failure) {
    switch (failure) {
        case HttpFailure::InvalidUrl: return FailureKind::Transport;
        case HttpFailure::Connect: return FailureKind::Transport;
        case HttpFailure::Timeout: return FailureKind::Timeout;
        case HttpFailure::Send: return FailureKind::Transport;
        case HttpFailure::Receive: return FailureKind::Transport;
        case HttpFailure::Redirect: return FailureKind::Redirect;
        case HttpFailure::Status: return FailureKind::HttpStatus;
        case HttpFailure::TooLarge: return FailureKind::TooLarge;
        case HttpFailure::Empty: return FailureKind::Validation;
        case HttpFailure::Aborted: return FailureKind::None;
        case HttpFailure::None: return FailureKind::None;
    }
    return FailureKind::Transport;
}

FailureKind failure_from_validation(ValidationKind kind) {
    switch (kind) {
        case ValidationKind::Encoding: return FailureKind::Encoding;
        case ValidationKind::TooLarge: return FailureKind::TooLarge;
        case ValidationKind::Parse: return FailureKind::Parse;
        default: return FailureKind::Validation;
    }
}

// View → Frame 的映射只有一份，在 display/frame.h（离线量具与主进程共用同一份实现，
// 免得“量具算出来的帧”与“主进程真的发出去的帧”各有一半解释）。这里不再自己抄一份。

class Master {
public:
    Master(Command command, std::wstring scope)
        : command_(std::move(command)), scope_(std::move(scope)) {
        no_taskbar_ = command_.no_taskbar;
        hidden_ = command_.hidden;
        // 失败注入点只在隔离实例里生效（见成员的说明）。
        test_fail_thread_ =
            !scope_.empty() && read_test_environment(L"DESKFEED_TEST_FAIL_FETCH_THREAD") == L"1";
        test_refuse_outcome_ =
            !scope_.empty() && read_test_environment(L"DESKFEED_TEST_REFUSE_OUTCOME") == L"1";
        // 第三个注入点：把**已投递**的退出消息延迟到 pump 里再消费（见 defer_quits_ 的成员说明）。
        // 只在“隔离实例 + 无界面”里生效，生产实例（scope 恒为空）打不开。
        if (!scope_.empty() && command_.headless) {
            std::wstring const value = read_test_environment(L"DESKFEED_TEST_DEFER_QUIT_MS");
            if (!value.empty()) {
                wchar_t* end = nullptr;
                long long const parsed = std::wcstoll(value.c_str(), &end, 10);
                if (end != value.c_str() && *end == L'\0' && parsed > 0) {
                    defer_quit_ms_ = static_cast<unsigned long>(
                        std::min<long long>(parsed, kMaxDeferQuitMs));
                    defer_quits_ = true;
                }
            }
        }
    }
    ~Master();

    int run();

private:
    struct Job {
        std::uint64_t id = 0;
        std::uint64_t generation = 0;
        HANDLE thread = nullptr;
        bool active = false;
        bool timed_out = false;
        std::shared_ptr<std::atomic<bool>> abort;
        // 工作线程报告“结果没能入队”：主进程据此立刻收尾这次取数（不必干等墙钟预算）。
        std::shared_ptr<std::atomic<bool>> finished_without_result;
    };

    // 第一阶段：**只做控制平面**。单实例仲裁已经在 master_main 里完成，这里建接收端、开日志、
    // 读设置文件。它绝不建显示面：不建悬浮窗、不建显示邮箱、不武装任务栏编排、不取数。
    //
    // 理由就是契约里的“先仲裁后副作用”：首份命令被拒绝（地址非法、PID 非法、落盘失败）时，
    // 这个进程除了一个控制窗口和一行日志之外什么都不该留下——尤其不能留下一个按生产命名
    // 建好的显示邮箱或一次已经发出去的注入。
    bool initialize_control();
    // 第二阶段：只有“首份状态已经被接受”之后才调用：
    //   * `--url` 命令：apply_command 返回 Accepted；
    //   * 无参数启动：保存的设置读出来了，URL 校验过（或明确判定为“未配置 URL”）。
    // 从这一刻起才有显示面：显示邮箱、悬浮窗、任务栏编排，以及随后的第一次取数。
    bool start_display_face();
    ControlReply on_control(ControlRequest const& request);
    ControlReply apply_command(Command const& command, std::uint32_t sender_pid);
    void start_job();
    void cancel_active_job();
    void reap_jobs();
    Job* find_job(std::uint64_t id);
    void on_fetch_done(std::unique_ptr<FetchOutcome> outcome);
    void on_binding_exit(std::uint64_t generation);
    // 退出请求的**唯一**消费点。`wanted` 是投递时记下的绑定代次，0 表示无条件退出。
    // 条件退出在这里**重新核验**：代次已经被更新的接管取代（或绑定已经不在了）就丢弃，
    // 绝不让一条旧的退出消息关掉新的绑定。核验通过才真正进入退出流程（closing_ + quit_requested_）。
    void consume_quit(std::uint64_t wanted);
    // 把延迟注入点里到期的退出消息按顺序交给 consume_quit（只在本隔离实例的测试里非空）。
    void drain_pending_quits(long long now);
    // 把当前显示模型发出去：显示邮箱与悬浮窗吃**同一份** Frame（frame_from_view 只算一次），
    // 各自按自己的“已发布代次”判断要不要发。force 用于“模型没变但读端需要一帧”的场合
    // （启动时、任务栏组件被重新打开/重新注入时）。
    void publish_display_outputs(bool force);

    // 悬浮窗交回来的四个动作（见 widget/window.h 的重入与生命周期契约）：它们都是同步调用的，
    // 实现里绝不销毁窗口、绝不重入 apply / show_from_request，也绝不让 SettingsFile 失效。
    void on_widget_refresh();
    void on_widget_quit();
    bool on_widget_set_taskbar_component(bool enabled);
    bool on_widget_save_preferences(widget::WindowPreferences const& preferences,
                                    std::wstring* error);
    // 组件开关的落盘：候选克隆 → 原子替换 → 成功才接过来（与 apply_command 同一套语义）。
    // 返回 false 时正在用的那一份一个字节都没被碰过。
    bool persist_component_intent(bool enabled);

    // 交给 WidgetCallbacks 的普通函数指针（context 就是 this）。
    static void widget_refresh_entry(void* context);
    static void widget_quit_entry(void* context);
    static bool widget_set_taskbar_component_entry(void* context, bool enabled);
    static bool widget_save_preferences_entry(void* context,
                                              widget::WindowPreferences const& preferences,
                                              std::wstring* error);
    // 主进程**唯一**那条状态推进逻辑：到期 tick、墙钟截止、结果出队、绑定退出、发帧。
    //
    // 它有两个入口，但只有一份实现：
    //   * 主循环每轮调一次（之后才进入阻塞等待）；
    //   * 控制窗口过程收到 WM_APP / 心跳 WM_TIMER 时调一次——**嵌套模态循环期间**（右键菜单
    //     的 TrackPopupMenu）主循环跑不到，这是唯一还能推进状态机的入口。
    // `pumping_` 是防重入闸门：模态循环里再派发到同一条消息时直接返回，绝不递归。
    void pump(bool from_modal_dispatch);
    void drain_outcomes();
    bool on_window_message(UINT message, WPARAM wparam, LPARAM lparam);
    void shutdown();
    std::string inspect_json() const;

    Command command_;
    std::wstring scope_;
    std::wstring class_name_;
    std::wstring settings_path_;
    std::wstring log_path_;
    ControlServer server_;
    std::unique_ptr<SettingsFile> settings_;
    bool settings_usable_ = true;

    ScheduleConfig schedule_{};
    std::wstring url_;
    bool configured_ = false;
    HealthState health_;
    ProcessBinding binding_;
    FetchScheduler scheduler_;

    std::vector<Job> jobs_;
    std::uint64_t next_job_id_ = 1;
    std::uint64_t active_job_id_ = 0;
    long long fetch_deadline_ms_ = 0;

    std::uint64_t source_generation_ = 0;
    std::uint64_t binding_generation_ = 0;
    // 绑定代次的分配计数器：**从不复用**。prepare 会立刻启动等待者，所以一次最终没有提交的
    // 尝试（落盘失败被 discard）也可能已经把自己的代次投递进消息队列，而 discard 收不回
    // 已排队的窗口消息。用 binding_generation_ + 1 的话，下一次成功的绑定会拿到同一个值，
    // 那条迟到的旧消息就会误伤新绑定。
    std::uint64_t next_binding_generation_ = 1;
    bool quit_requested_ = false;
    // “退出已经排定、本实例不会再继续服务”的硬标志。与 quit_requested_ 分开：
    //   * 无条件退出（`--quit`、悬浮窗菜单「退出」）在**返回接受之前**就置位——不能先 ACK
    //     “命令已接受”，随后又明知自己在退出还去接受一条新的接管；
    //   * 条件退出（绑定生产者退出 / `--quit-if-bound`）**不**在投递时置位：那条消息带着绑定代次，
    //     允许被随后到达的新绑定取消（契约：被新生产者接管则不退出）。只有它真的被消费、退出
    //     已经不可撤销时才置位。
    // 置位之后 `on_control` 拒绝新的 Apply（返回 Rejected，绝不假 Accepted）。
    bool closing_ = false;
    // 测试注入点：把已投递的退出消息**延迟**到 pump 里再消费（`DESKFEED_TEST_DEFER_QUIT_MS`，
    // 只对 scope 非空 + headless 生效）。它把“退出消息已经投递、但还没被消费”这段窗口从几微秒
    // 变成确定的若干毫秒，用例才能稳定地在这段时间里提交一次新接管，从而真正分辨
    // “条件退出带不带代次”。生产实例（scope 恒为空）不可能打开它。
    bool defer_quits_ = false;
    unsigned long defer_quit_ms_ = 0;
    struct PendingQuit {
        std::uint64_t wanted = 0;
        long long due_ms = 0;
    };
    std::vector<PendingQuit> pending_quits_;
    // 任务栏那块的双击/「打开桌面悬浮窗」把这条置起来。主循环据此请悬浮窗显示自己
    // （它会顺手把用户的隐藏意图清掉落盘）。它**不**触发一次新的取数：显示是显示，取数是取数。
    bool show_requested_ = false;
    bool no_taskbar_ = false;
    bool hidden_ = false;

    // 任务栏组件刻意分成三件事（缺一个就会写错状态）：
    //   * component_actual_：**用户/磁盘上的意图**（`[taskbar] component`，缺省 true）。
    //   * component_capability_：**本次运行有没有能力**提供它——用户没关掉任务栏、显示邮箱
    //     建起来了、注入器也真的在。它决定菜单项灰不灰（WidgetOptions.taskbar_available）。
    //   * component_effective_ = actual && capability：**现在到底是不是开着**（菜单的勾、
    //     也是交给编排层的目标状态）。
    // 把 capability 当成“当前开关”是错的：那样用户一关组件，菜单项就永久置灰，再也打不开。
    bool component_actual_ = true;
    bool component_capability_ = false;
    bool component_effective_ = false;
    TaskbarSupervisor supervisor_;

    // 悬浮窗。窗口、邮箱与主进程同一条线程：窗口的 HWND 建在主进程这条线程上，主循环的
    // MsgWaitForMultipleObjects + PeekMessage 已经会泵它的消息，不需要第二条消息循环。
    // widget_revision_ 是窗口那一份“已发布代次”。
    widget::WidgetWindow widget_;
    bool widget_created_ = false;
    std::uint64_t widget_revision_ = 0;

    // 显示邮箱：显式要求（--frame-mailbox）或“本次运行真的有任务栏显示面”时才建。
    // 建起来之后它是主进程 → 负载 DLL 唯一的输出通道；发布失败只记日志，绝不影响取数。
    DisplayFrameProducer frame_producer_;
    bool mailbox_ok_ = false;
    std::uint64_t published_revision_ = 0;

    // 取数结果队列（工作线程入队、主进程出队）。主进程持一份所有权，每个工作线程也持一份：
    // 主进程收尾时只停止接受并丢掉自己那一份，队列本身活到最后一个线程 return。
    std::shared_ptr<OutcomeQueue> outcomes_ = OutcomeQueue::create();
    bool pumping_ = false;

    // 测试用的失败注入点：让 start_job() 走 CreateThread 失败那条收尾路径。
    //
    // 只在**隔离实例**里生效（scope 非空）：生产实例的 scope 恒为空，所以这个开关在生产上
    // 不可能被打开，也就不会成为一个“远程可触发”的行为。它的价值是让“start_job 当场改模型、
    // 而这次改动必须在阻塞等待之前发布”这条时序有一个可复现的用例，而不是只靠读代码断言。
    bool test_fail_thread_ = false;
    // 第二个注入点：让工作线程的入队必定失败（`DESKFEED_TEST_REFUSE_OUTCOME=1`），用来永久
    // 覆盖 “finished_without_result ⇒ pump 立刻按内部失败收尾、不等墙钟预算” 这条路径。
    // 同样只在 scope 非空时生效。它不伪造结果、不改变线程的真实出口，只是让“队列拒绝”这个
    // 分支在一个用例里必然发生。
    bool test_refuse_outcome_ = false;
};

Master::~Master() {
    for (Job& job : jobs_) {
        if (job.thread != nullptr) {
            if (job.abort) {
                job.abort->store(true);
            }
            ::WaitForSingleObject(job.thread, kShutdownJoinMs);
            ::CloseHandle(job.thread);
            job.thread = nullptr;
        }
    }
    jobs_.clear();
    outcomes_->stop_accepting();  // 停止接受新结果；队列本身由 shared_ptr 活到最后一个线程结束
    outcomes_.reset();
}

bool Master::initialize_control() {
    class_name_ = scoped_name(kControlWindowClassBase, scope_);
    settings_path_ = command_.state_dir.empty() ? deskfeed_settings_path(scope_)
                                                : command_.state_dir + L"\\settings.ini";
    log_path_ = deskfeed_log_path(scope_);

    // 顺序按契约：先仲裁（已在 master_main 完成）与接收端就绪，再截断日志、再动网络。
    std::wstring directory_error;
    if (!ensure_parent_directory(log_path_, directory_error)) {
        console_write_stderr(L"deskfeed: " + directory_error + L"\n");
        return false;
    }
    std::string error;
    if (!server_.create(class_name_, nullptr, error)) {
        console_write_stderr(L"deskfeed: 建不了控制窗口：" + wide_from_utf8(error) + L"\n");
        return false;
    }
    server_.set_handler([this](ControlRequest const& request) { return on_control(request); });
    // 主进程自己的窗口消息与心跳都走同一个窗口过程：主循环的 PeekMessage 与嵌套模态循环
    // （右键菜单）的派发都会到这里，所以状态机在两种循环下都能继续推进。
    server_.set_pump(
        [this](UINT message, WPARAM wparam, LPARAM lparam) {
            return on_window_message(message, wparam, lparam);
        });
    bool const have_heartbeat = server_.start_heartbeat(kHeartbeatMs);
    if (!have_heartbeat && !command_.headless) {
        // 心跳是**嵌套模态循环**（右键菜单的 TrackPopupMenu）期间唯一还能推进状态机的入口：
        // 模态期间主循环那次 PeekMessage 根本跑不到。没有它，菜单开着的时候取数结果、到期与
        // 整体失效都不会被发布，两个显示面会各自停在旧状态上——而契约明确要求到期由主进程
        // 独立判定、两面同状态。这不是可以默默降级的东西：要开悬浮窗就可能出现模态菜单，
        // 建不出心跳就只能 fail closed。（无界面运行没有模态循环，心跳本来也用不上，见下。）
        console_write_stderr(
            L"deskfeed: 心跳定时器建不起来；带界面运行时模态菜单期间无法推进状态机，拒绝启动\n");
        server_.destroy();
        return false;
    }

    if (!log_open(log_path_, false)) {
        console_write_stderr(L"deskfeed: 建不了日志文件 " + log_path_ + L"\n");
        return false;
    }
    if (!have_heartbeat) {
        // 只有**无界面**运行会走到这里：没有悬浮窗就没有模态菜单，主循环仍按精确的到期时刻醒来，
        // 控制窗口过程也照样在 WM_APP 上推进状态。这是有界、可解释的降级，不是“正常服务”。
        log_line(L"deskfeed: 心跳定时器建不起来；本次运行没有界面，不存在模态循环，按无界面继续");
    }
    log_format(L"deskfeed: master starting (pid=%lu scope=%s class=%s)",
               ::GetCurrentProcessId(), scope_.empty() ? L"(production)" : scope_.c_str(),
               class_name_.c_str());

    settings_ = std::make_unique<SettingsFile>(settings_path_);
    std::wstring settings_error;
    if (!settings_->load(settings_error)) {
        settings_usable_ = false;
        log_line(L"settings: " + settings_error + L"；本次运行不会写配置");
    }
    if (no_taskbar_) {
        log_line(L"deskfeed: --no-inject 生效：本次运行不碰任务栏");
    }
    if (hidden_) {
        log_line(L"deskfeed: --hidden 生效：本次运行悬浮窗保持隐藏");
    }
    // 到这里为止只有控制平面：控制窗口、心跳、日志、设置文件。**没有**显示邮箱、悬浮窗、
    // 任务栏编排，也没有第一次取数——那些都在 start_display_face() 里，只有首份状态被
    // 接受之后才建。首份 `--url` 命令被拒绝时，这个进程不该留下任何显示面或注入副作用。
    return true;
}

bool Master::start_display_face() {
    // 先把“能力”算清楚再动界面：菜单项灰不灰、编排层武装不武装、悬浮窗拿到的勾是什么，
    // 全都取决于它。
    bool const taskbar_wanted = !command_.headless && !no_taskbar_;

    // 1. 显示邮箱。两种情况要建：显式要求（--frame-mailbox，隔离的离线验收通道），或本次运行
    //    真的有任务栏显示面（负载 DLL 是邮箱唯一的读者）。它只建一块按 scope 命名的共享内存
    //    与一个通知事件，不碰界面、不注入——但只在首份状态被接受之后才建。
    if (command_.frame_mailbox || taskbar_wanted) {
        UserSecurity security;
        if (!security.build()) {
            log_line(L"display: 建不了用户安全描述符，显示邮箱改用系统默认 ACL");
        }
        std::string mailbox_error;
        if (frame_producer_.create(scope_, &security, mailbox_error)) {
            mailbox_ok_ = true;
            log_line(command_.frame_mailbox
                         ? L"display: 显示邮箱已建立（--frame-mailbox：只建隔离的命名对象，不碰界面）"
                         : L"display: 显示邮箱已建立（任务栏负载的显示帧通道）");
        } else {
            std::wstring const message = wide_from_utf8(mailbox_error);
            if (command_.frame_mailbox) {
                // 显式要求了这条 seam，建不起来就是失败：静默继续会让离线验收“跑通了但什么也没验”。
                console_write_stderr(L"deskfeed: 建不了显示邮箱：" + message + L"\n");
                log_line(L"display: 建不了显示邮箱：" + message);
                return false;
            }
            // 生产路径：邮箱建不起来只说明“这次没有任务栏显示面”，不该拦住取数与悬浮窗。
            log_line(L"display: 建不了显示邮箱：" + message + L"；本次运行不提供任务栏组件");
        }
    }

    // 2. 任务栏编排。武装它之前先把“注入器在不在”问清楚：这个答案本身就是能力的一部分。
    //    start() 只记配置并检查文件，**不 spawn 任何东西**；组件本来就关着时它也一次动作都不发。
    component_actual_ = read_taskbar_component_intent(settings_.get());
    // 初始目标状态 = 用户的持久意图 **且** 这次真的有显示面（邮箱建起来了）。
    // 少了后半截就是一种很难看的错法：邮箱没建起来（菜单也因为能力不成立而置灰），但编排层已经
    // 被武装成“开”，pump 每轮 tick 都会去 spawn `--load`——装进去的负载没有帧可读，只会一直
    // 失败重试。**没有邮箱就一个动作都不许发**：下面干脆连武装都不做。
    bool const taskbar_enabled_at_start = component_actual_ && mailbox_ok_;
    if (taskbar_wanted && mailbox_ok_) {
        TaskbarSupervisorOptions options;
        options.scope = scope_;
        options.master_pid = ::GetCurrentProcessId();
        std::string error;
        if (supervisor_.start(options, taskbar_enabled_at_start, error)) {
            log_line(L"taskbar: 任务栏编排已武装");
        } else {
            log_line(L"taskbar: " + wide_from_utf8(error) + L"；本次运行不提供任务栏组件");
        }
    } else if (taskbar_wanted) {
        // 没有显示邮箱：不进这条路径（不是“武装成关”，是根本不存在）。tick 对未武装的编排层
        // 是空操作，所以“绝不盲装负载”这件事是结构上成立的，不靠一个可能被写错的标志位。
        log_line(L"taskbar: 没有显示邮箱，本次运行不武装任务栏编排（绝不盲装负载）");
    }
    component_capability_ = taskbar_wanted && mailbox_ok_ && supervisor_.started();
    component_effective_ = taskbar_enabled_at_start && supervisor_.started();

    // 3. 悬浮窗。--headless / --mock 不建（命令解析已经把 hidden 一起置上）。
    if (!command_.headless) {
        widget::WidgetOptions options;
        options.instance = ::GetModuleHandleW(nullptr);
        options.class_name = scoped_name(kWidgetWindowClassBase, scope_);
        // 这一项是**能力**，不是当前开关。用 effective 去填就错了：用户一关组件，菜单项就永久
        // 置灰，再也没有把它打开的办法。能力在（注入器在、邮箱在、没关任务栏）就不置灰。
        options.taskbar_available = component_capability_;
        options.force_hidden = command_.hidden;  // --hidden 是本次运行的开关，绝不落盘
        options.preferences = settings_ ? widget::read_window_preferences(*settings_)
                                        : widget::WindowPreferences{};
        // 读到的 `taskbar_component` 缺省是 true；这里按主进程这份 effective 状态填，它是
        // 菜单的勾、也是交给编排层的目标状态。
        options.preferences.taskbar_component = component_effective_;

        widget::WidgetCallbacks callbacks;
        callbacks.context = this;
        callbacks.refresh = &Master::widget_refresh_entry;
        callbacks.quit = &Master::widget_quit_entry;
        callbacks.set_taskbar_component = &Master::widget_set_taskbar_component_entry;
        callbacks.save_preferences = &Master::widget_save_preferences_entry;

        std::wstring error;
        if (widget_.create(options, callbacks, &error)) {
            widget_created_ = true;
            log_line(L"widget: 悬浮窗已建立");
        } else {
            // 建不了窗口不是致命失败：取数与任务栏那块是独立的两条显示面，照样跑。
            log_line(L"widget: 建不了悬浮窗：" + error + L"；主进程继续运行");
        }
    }

    log_format(L"deskfeed: display face ready (mailbox=%s widget=%s taskbar=%s component=%s)",
               mailbox_ok_ ? L"yes" : L"no", widget_created_ ? L"yes" : L"no",
               component_capability_ ? L"yes" : L"no", component_effective_ ? L"on" : L"off");
    return true;
}

void Master::publish_display_outputs(bool force) {
    std::uint64_t const revision = health_.revision();
    bool const mailbox_due =
        frame_producer_.created() && (force || revision != published_revision_);
    bool const widget_due = widget_created_ && (force || revision != widget_revision_);
    if (!mailbox_due && !widget_due) {
        return;  // 模型没变：不往跨进程 seqlock 里白写一帧，也不白刷一次窗口
    }
    // 两个显示面吃**同一份** Frame：各算一遍是显示分叉的源头，也是“一面正常、一面失效”的来源。
    Frame const frame = frame_from_view(health_.view());
    if (mailbox_due) {
        if (frame_producer_.publish(frame, wall_ms())) {
            published_revision_ = revision;
        } else {
            // 失败**不推进**代次：下一轮还会重发，不会把一次失败的发布当成已经发出去了。
            log_line(L"display: 发布显示帧失败（不影响取数）");
        }
    }
    if (widget_due) {
        widget_.apply(frame);
        widget_revision_ = revision;
    }
}

// ---------- 悬浮窗交回来的四个动作 ----------
//
// 它们都在主进程这条线程上被窗口**同步**调用（见 widget/window.h 的契约），所以：
//   * 绝不在这里销毁窗口、也绝不重入 apply / show_from_request；
//   * 只投递消息或做一次有界的落盘，不做网络；
//   * `settings_` 在窗口 destroy() 返回之前一直有效（Master 不提前释放它）。

void Master::widget_refresh_entry(void* context) {
    auto* self = static_cast<Master*>(context);
    if (self != nullptr) {
        self->on_widget_refresh();
    }
}

void Master::on_widget_refresh() {
    // 只投递一条消息：这里还在窗口过程里，而取数要开线程。真正的取数在主循环里做，
    // 而且它**不改地址、不改调度配置、不重绑**。
    ::PostMessageW(server_.window(), kMsgRefreshRequested, 0, 0);
}

void Master::widget_quit_entry(void* context) {
    auto* self = static_cast<Master*>(context);
    if (self != nullptr) {
        self->on_widget_quit();
    }
}

void Master::on_widget_quit() {
    // 同样只投递：在回调里同步销毁窗口会在窗口过程还压在栈上的时候把窗口拆掉。
    // 菜单「退出」是**无条件**退出：先把 closing_ 置上（本实例不会再接受新接管），再投递。
    closing_ = true;
    ::PostMessageW(server_.window(), kMsgQuit, 0, 0);
}

bool Master::widget_set_taskbar_component_entry(void* context, bool enabled) {
    auto* self = static_cast<Master*>(context);
    return self != nullptr && self->on_widget_set_taskbar_component(enabled);
}

bool Master::on_widget_set_taskbar_component(bool enabled) {
    if (!component_capability_) {
        // 能力不成立（--no-inject / 找不到注入器 / 邮箱建不起来）：菜单这一项本来就是灰的。
        // 这里绝不产生任何副作用，也绝不谎称“打开了”。
        return false;
    }
    if (!persist_component_intent(enabled)) {
        // 没落盘：实际状态一个字节都没变，如实把**当前**状态交回去，菜单的勾才不会骗人。
        return component_effective_;
    }
    component_actual_ = enabled;
    component_effective_ = component_actual_ && component_capability_;
    // 只改编排层的目标状态：它不阻塞、不等注入器，真正的注入/卸载由 tick 异步驱动。
    supervisor_.set_enabled(component_effective_);
    log_format(L"taskbar: the component switch is now %s (persisted)",
               component_effective_ ? L"on" : L"off");
    return component_effective_;
}

bool Master::persist_component_intent(bool enabled) {
    if (settings_ == nullptr || !settings_usable_) {
        // 设置文件不可用（读不出来 = 行是空的）：写下去会把用户的文件覆盖成只剩这一行。
        log_line(L"taskbar: 设置文件不可用，组件开关不落盘");
        return false;
    }
    // 与 apply_command 同一套事务语义：候选克隆 → 原子替换 → 成功才接过来。写失败的候选
    // 绝不会把正在用的那一份（URL / 调度）带脏。
    SettingsFile candidate = *settings_;
    std::wstring error;
    candidate.set(L"taskbar", L"component", enabled ? L"true" : L"false", nullptr, error);
    if (!candidate.save_as(settings_->path(), error)) {
        log_line(L"taskbar: 保存组件开关失败：" + error + L"；实际状态不变");
        return false;
    }
    settings_->adopt(candidate);
    return true;
}

bool Master::widget_save_preferences_entry(void* context,
                                           widget::WindowPreferences const& preferences,
                                           std::wstring* error) {
    auto* self = static_cast<Master*>(context);
    return self != nullptr && self->on_widget_save_preferences(preferences, error);
}

bool Master::on_widget_save_preferences(widget::WindowPreferences const& preferences,
                                        std::wstring* error) {
    if (settings_ == nullptr || !settings_usable_) {
        if (error != nullptr) {
            *error = L"设置文件不可用";
        }
        return false;
    }
    // 界面偏好只写 `[window]` 一节；`[taskbar] component` 由组件开关那条路径自己写一次。
    // 同样在**候选克隆**上写：write_window_preferences 失败时可能会重新 load，直接对正在用的
    // 那一份调用会把内存里的 URL / 调度拖进一次可能也失败的回滚里。
    SettingsFile candidate = *settings_;
    std::wstring local_error;
    if (!widget::write_window_preferences(candidate, preferences, &local_error)) {
        if (error != nullptr) {
            *error = local_error;
        }
        return false;
    }
    settings_->adopt(candidate);
    return true;
}

ControlReply Master::on_control(ControlRequest const& request) {
    if (request.op == ControlOp::Inspect) {
        std::string const json = inspect_json();
        std::wstring const path = deskfeed_inspect_path(scope_, request.sender_pid);
        std::wstring error;
        if (!write_file_bytes(path, json, error)) {
            log_line(L"inspect: " + error);
            return ControlReply::Rejected;
        }
        log_format(L"inspect: wrote %s (%zu bytes)", path.c_str(), json.size());
        return ControlReply::Accepted;
    }

    // 任务栏组件的两个动作请求：只投递一条消息，立刻应答。
    //
    // 这里**不做**任何网络、磁盘或注入工作，也不等待：窗口过程跑在 SendMessageTimeoutW 的
    // 调用方线程上，DLL 那边即使是从自己的 worker 上发出来的，也不该让它按别人的预算挂住。
    // Accepted 只表示“动作已经排进主进程的消息队列”，取数是否成功要由显示帧自己说明。
    if (request.op == ControlOp::Refresh || request.op == ControlOp::ShowWidget) {
        if (!request.args.empty()) {
            log_line(L"control: 任务栏动作请求带了多余参数，拒绝");
            return ControlReply::BadRequest;
        }
        bool const refresh = request.op == ControlOp::Refresh;
        if (!::PostMessageW(server_.window(), refresh ? kMsgRefreshRequested : kMsgShowRequested, 0,
                            0)) {
            log_line(L"control: 任务栏动作请求投递失败");
            return ControlReply::Rejected;
        }
        log_format(L"control: 任务栏动作请求已排队（%s，来自 pid %lu）",
                   refresh ? L"刷新读数" : L"打开悬浮窗", request.sender_pid);
        return ControlReply::Accepted;
    }

    Command command;
    std::wstring error;
    if (!command_parse(request.args, command, error)) {
        log_line(L"control: 参数不合法：" + error);
        return ControlReply::BadRequest;
    }
    switch (command.kind) {
        case CommandKind::QuitIfBound:
            if (!binding_.active() || binding_.pid() != command.pid) {
                log_format(L"control: --quit-if-bound %lu 不匹配（当前绑定 %s）", command.pid,
                           binding_.active() ? std::to_wstring(binding_.pid()).c_str() : L"无");
                return ControlReply::NotBound;
            }
            log_format(L"control: --quit-if-bound %lu 匹配；准备正常退出", command.pid);
            // 条件退出：消息带着当前绑定代次。**不**在这里置 closing_——随后到达的新接管
            // （新生产者）应当能取消这次退出，契约要求“被接管则不退出”。
            ::PostMessageW(server_.window(), kMsgQuit,
                           static_cast<WPARAM>(binding_generation_), 0);
            return ControlReply::Accepted;
        case CommandKind::Quit:
            log_line(L"control: --quit；准备正常退出");
            // 无条件退出：在返回接受**之前**就置 closing_。这是诚实的顺序——既然这条命令已经被
            // 接受、本实例马上要退出，就不能让紧接着到达的接管拿到一个“已接受”的假确认。
            closing_ = true;
            ::PostMessageW(server_.window(), kMsgQuit, 0, 0);
            return ControlReply::Accepted;
        case CommandKind::Apply:
            if (closing_) {
                // 退出已经排定（无条件退出已确认，或条件退出已经被消费）：本实例不会再继续服务，
                // 拒绝这条接管并如实返回 Rejected，绝不 ACK Accepted 之后马上消失。
                log_line(L"control: 实例已在退出流程里，拒绝新的接管（不假称已接受）");
                return ControlReply::Rejected;
            }
            return apply_command(command, request.sender_pid);
        case CommandKind::Start:
            log_line(L"control: 无参数命令：不改变地址与绑定");
            return ControlReply::Accepted;
        case CommandKind::Inspect:
        case CommandKind::Help:
            return ControlReply::BadRequest;
    }
    return ControlReply::BadRequest;
}

ControlReply Master::apply_command(Command const& command, std::uint32_t sender_pid) {
    // 1. 地址
    UrlParts parts;
    std::wstring url_error;
    if (!url_crack(command.url, parts, url_error)) {
        log_line(L"apply: 地址被拒绝：" + url_error);
        return ControlReply::Rejected;
    }

    // 2. 生产者句柄（准备阶段：失败时不碰旧绑定）
    unsigned long const pid = command.has_pid ? command.pid : 0;
    std::uint64_t const next_binding_generation = next_binding_generation_++;
    ProcessBinding::Prepared prepared;
    if (command.has_pid) {
        if (pid == 0 || pid == 4 || pid == ::GetCurrentProcessId() ||
            (sender_pid != 0 && pid == sender_pid)) {
            log_format(L"apply: PID %lu 不合法（0 / System / 本进程 / 转发进程都不接受）", pid);
            return ControlReply::Rejected;
        }
        HANDLE const process = ::OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (process == nullptr) {
            log_format(L"apply: 打不开 PID %lu：%s", pid, last_error_text().c_str());
            return ControlReply::Rejected;
        }
        std::string binding_error;
        if (!ProcessBinding::prepare(process, pid, next_binding_generation, server_.window(),
                                     kMsgBindingExit, prepared, binding_error)) {
            ::CloseHandle(process);  // 失败时句柄仍归调用方
            log_format(L"apply: 绑定 PID %lu 失败：%s", pid, wide_from_utf8(binding_error).c_str());
            return ControlReply::Rejected;
        }
    }

    // 3. 落盘：**候选克隆 + 原子替换 + 接过来**（先落盘、后提交内存）。
    //
    // 为什么不是“直接在正在用的这一份上改，失败再 load 回来”：save 失败时那一份已经带着被拒
    // 的候选值了，而“读回磁盘状态”这次回滚本身也可能失败（盘打不开、文件被占），于是内存里
    // 会留下一份既没落盘、又不像旧的脏状态。这里改成在**克隆**上改，落盘成功了才把克隆接进
    // 正在用的这一份；任何一步失败，正在用的那一份一个字节都没被碰过。
    bool changed = false;
    std::wstring settings_error;
    if (!settings_usable_) {
        ProcessBinding::discard(prepared);
        log_line(L"apply: 设置文件不可用，拒绝整条命令");
        return ControlReply::Rejected;
    }
    SettingsFile candidate = *settings_;
    candidate.set(L"source", L"url", command.url, &changed, settings_error);
    if (command.has_poll_ms) {
        candidate.set(L"source", L"poll_ms",
                      std::to_wstring(schedule_clamp_poll(command.poll_ms).value), &changed,
                      settings_error);
    }
    if (command.has_retry_ms) {
        candidate.set(L"source", L"retry_ms",
                      std::to_wstring(schedule_clamp_retry(command.retry_ms).value), &changed,
                      settings_error);
    }
    if (command.has_timeout_ms) {
        candidate.set(L"source", L"timeout_ms",
                      std::to_wstring(schedule_clamp_timeout(command.timeout_ms).value), &changed,
                      settings_error);
    }
    if (!candidate.save_as(settings_->path(), settings_error)) {
        ProcessBinding::discard(prepared);
        log_line(L"apply: 保存设置失败：" + settings_error + L"；拒绝整条接管");
        // 正在用的那一份没被改过，不需要“回读”这种可能再失败一次的回滚。
        return ControlReply::Rejected;
    }
    settings_->adopt(candidate);  // 落盘成功：把候选接过来（内存与磁盘一致，且不回读）

    // 4. 提交运行时状态
    ScheduleConfig next_schedule = schedule_;
    if (command.has_poll_ms) {
        next_schedule.poll_ms = schedule_clamp_poll(command.poll_ms).value;
    }
    if (command.has_retry_ms) {
        next_schedule.retry_ms = schedule_clamp_retry(command.retry_ms).value;
    }
    if (command.has_timeout_ms) {
        next_schedule.timeout_ms = schedule_clamp_timeout(command.timeout_ms).value;
    }

    bool const url_changed = command.url != url_;
    schedule_ = next_schedule;
    scheduler_.set_config(schedule_);
    binding_generation_ = next_binding_generation;
    if (command.has_pid) {
        binding_.commit(prepared);
    } else {
        binding_.clear();
    }

    if (url_changed) {
        // 换源：清空旧源缓存、丢弃旧源在途响应，并立刻取新源。
        cancel_active_job();
        ++source_generation_;
        url_ = command.url;
        configured_ = true;
        health_.set_configured(true);
        scheduler_.reset(monotonic_ms());
        log_format(L"apply: 地址已切换为 %s（source generation %llu）", redact(url_).c_str(),
                   source_generation_);
    } else {
        url_ = command.url;
        if (!configured_) {
            configured_ = true;
            health_.set_configured(true);
            scheduler_.reset(monotonic_ms());
        }
        log_format(L"apply: 地址未变（%s），按命令要求执行了重绑（binding generation %llu）",
                   redact(url_).c_str(), binding_generation_);
    }
    if (command.has_pid) {
        log_format(L"apply: 绑定生产者 PID %lu", pid);
    } else {
        log_line(L"apply: 已解除生产者绑定，转为独立运行");
    }
    return ControlReply::Accepted;
}

Master::Job* Master::find_job(std::uint64_t id) {
    for (Job& job : jobs_) {
        if (job.id == id) {
            return &job;
        }
    }
    return nullptr;
}

void Master::start_job() {
    // 先把这一次取数需要的元数据全部复制成局部量，再交给线程：
    // 线程一跑起来就会拿 context，并在结束时删掉它，之后再读 context 就是 use-after-free。
    std::uint64_t const job_id = next_job_id_++;
    std::uint64_t const generation = source_generation_;
    std::wstring const url = url_;
    HttpLimits const limits{kMaxBodyBytes, kMaxRedirects, schedule_.timeout_ms};
    std::shared_ptr<std::atomic<bool>> abort = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> finished_without_result =
        std::make_shared<std::atomic<bool>>(false);

    auto* context = new FetchJobContext();
    context->job_id = job_id;
    context->generation = generation;
    context->url = url;
    context->limits = limits;
    context->abort = abort;
    context->notify = server_.window();
    context->queue = outcomes_;  // 线程自己持有一份队列所有权（见 FetchJobContext 的说明）
    context->finished_without_result = finished_without_result;
    context->refuse_outcome = test_refuse_outcome_;

    // 失败注入点（见成员说明）在**创建线程之前**就分流：绝不允许“线程已经跑起来、再假装它
    // 没起来”。那样做等于让工作线程去读一个马上被 delete 的 context（真实的 use-after-free），
    // 测出来的就不是“创建失败这条收尾路径”，而是一个自己造出来的崩溃。
    HANDLE const thread = test_fail_thread_
                              ? nullptr
                              : ::CreateThread(nullptr, 0, &fetch_thread_entry, context, 0, nullptr);
    if (thread == nullptr) {
        if (test_fail_thread_) {
            log_line(L"fetch: 测试失败注入点生效，按 CreateThread 失败处理（未创建线程）");
        } else {
            log_format(L"fetch: CreateThread 失败：%s", last_error_text().c_str());
        }
        delete context;
        health_.apply_failure(FailureKind::Internal, "无法启动取数线程", wall_ms());
        scheduler_.on_result(false, monotonic_ms());
        return;
    }
    Job job;
    job.id = job_id;
    job.generation = generation;
    job.thread = thread;
    job.active = true;
    job.abort = abort;
    job.finished_without_result = finished_without_result;
    jobs_.push_back(std::move(job));
    active_job_id_ = job_id;
    fetch_deadline_ms_ = monotonic_ms() + schedule_.timeout_ms;
    log_format(L"fetch: 开始第 %llu 次取数（job %llu，预算 %lld ms，地址 %s）", generation, job_id,
               schedule_.timeout_ms, redact(url_).c_str());
}

void Master::cancel_active_job() {
    if (active_job_id_ == 0) {
        return;
    }
    if (Job* job = find_job(active_job_id_)) {
        job->active = false;
        job->timed_out = true;
        if (job->abort) {
            job->abort->store(true);
        }
    }
    active_job_id_ = 0;
}

void Master::reap_jobs() {
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        // 只回收“结果已经处理过或已经放弃”的作业。活动作业即使线程已经结束，
        // 也要等它那条结果消息被处理完（那时 active 才会变 false），否则会把有效结果误丢。
        if (!it->active && ::WaitForSingleObject(it->thread, 0) == WAIT_OBJECT_0) {
            ::CloseHandle(it->thread);
            it = jobs_.erase(it);
        } else {
            ++it;
        }
    }
}

void Master::on_fetch_done(std::unique_ptr<FetchOutcome> outcome) {
    if (!outcome) {
        return;
    }
    Job* job = find_job(outcome->job_id);
    bool const relevant = job != nullptr && job->active && !job->timed_out &&
                          outcome->job_id == active_job_id_ &&
                          outcome->generation == source_generation_ &&
                          outcome->http.failure != HttpFailure::Aborted;
    if (!relevant) {
        log_format(L"fetch: 丢弃迟到结果（job %llu，source generation %llu，当前 %llu）",
                   outcome->job_id, outcome->generation, source_generation_);
        return;
    }
    job->active = false;
    active_job_id_ = 0;
    long long const now = monotonic_ms();
    long long const wall = wall_ms();

    if (outcome->http.ok && outcome->validation.ok) {
        health_.apply_snapshot(outcome->snapshot, wall);
        scheduler_.on_result(true, now);
        log_format(L"fetch: 成功，%zu 条%s", outcome->snapshot.items.size(),
                   outcome->snapshot.has_expires_at ? L"，带到期时间" : L"");
        return;
    }

    FailureKind kind = FailureKind::Transport;
    std::string reason;
    if (!outcome->http.ok) {
        kind = failure_from_http(outcome->http.failure);
        reason = outcome->http.error;
        if (kind == FailureKind::None) {
            return;  // 主动取消，不当作失败
        }
    } else {
        kind = failure_from_validation(outcome->validation.kind);
        reason = outcome->validation.message;
    }
    health_.apply_failure(kind, reason, wall);
    scheduler_.on_result(false, now);
    log_format(L"fetch: 失败（%s）：%s", wide_from_utf8(failure_kind_name(kind)).c_str(),
               wide_from_utf8(reason).c_str());
}

void Master::on_binding_exit(std::uint64_t generation) {
    if (generation != binding_generation_ || !binding_.active()) {
        log_format(L"binding: 忽略旧代次的退出回调（generation %llu，当前 %llu）", generation,
                   binding_generation_);
        return;
    }
    // 代次对得上还不够：当前绑定的进程句柄必须真的已经触发。一次没提交的绑定尝试留下的
    // 迟到消息会带着一个“看起来对得上”的代次到达，只按代次判定会误退新实例。
    if (!binding_.signaled()) {
        log_format(L"binding: 代次 %llu 的回调到达，但当前绑定的 PID %lu 仍在运行；忽略",
                   generation, binding_.pid());
        return;
    }
    log_format(L"binding: 绑定生产者 PID %lu 已退出；实例准备正常退出", binding_.pid());
    // 退出请求带着**产生它的那个绑定代次**。判定放在这里、动作却要等到消息被消费，中间可能有
    // 一次新的接管把 binding_generation_ 推进——那时这条消息必须被丢弃（见 consume_quit）。
    ::PostMessageW(server_.window(), kMsgQuit, static_cast<WPARAM>(generation), 0);
}

void Master::consume_quit(std::uint64_t wanted) {
    // wanted == 0 是无条件退出（`--quit` / 悬浮窗菜单），不得被任何新绑定取消。
    // wanted != 0 是条件退出：只有它投递时对应的绑定**仍然是当前绑定**时才作数。
    // 这里必须重新核验，不能信任投递时的判定：中间可能已经有新生产者接管（binding_generation_
    // 被推进、binding_ 被换成新句柄），旧生产者的退出不得关闭当前实例。
    if (wanted != 0 && (wanted != binding_generation_ || !binding_.active())) {
        log_format(L"quit: 丢弃已被新绑定取代的条件退出（generation %llu，当前 %llu）", wanted,
                   binding_generation_);
        return;
    }
    closing_ = true;
    quit_requested_ = true;
    // 悬浮窗的右键菜单用 TrackPopupMenu 在本线程上开了一个**嵌套模态循环**；主循环在它返回
    // 之前跑不到 quit_requested_ 的判定处。EndMenu 结束的是**调用线程的**活动菜单——也就是我们
    // 自己那个窗口的菜单；没有活动菜单时它是无操作（返回值在这里不代表任何语义，所以不据此下结论）。
    // 它不会、也不该去动别的进程的窗口/菜单（别的进程的模态菜单本来就不阻塞我们的线程），
    // 所以不必枚举桌面、也不对别人的 HWND 下手。
    ::EndMenu();
    log_line(L"quit: 已调用 EndMenu 结束本线程可能存在的活动菜单");
}

void Master::drain_pending_quits(long long now) {
    if (pending_quits_.empty()) {
        return;
    }
    std::vector<PendingQuit> ready;
    std::vector<PendingQuit> waiting;
    for (PendingQuit const& item : pending_quits_) {
        (item.due_ms <= now ? ready : waiting).push_back(item);
    }
    pending_quits_.swap(waiting);
    for (PendingQuit const& item : ready) {
        consume_quit(item.wanted);  // 仍按原代次核验；被新绑定取代的照样丢弃
    }
}

void Master::shutdown() {
    log_line(L"deskfeed: shutting down");
    // 心跳先停：收尾过程中再触发一次 pump 只会往一条正在拆的路径上加事。
    server_.stop_heartbeat();
    // 取消是真的：HTTP 层每隔约 50 ms 看一次取消标志，并会关掉在途的请求句柄，
    // 所以这里的等待通常几毫秒就结束。上限只是兜底：超时也只记日志，不阻塞退出。
    for (Job& job : jobs_) {
        if (job.abort) {
            job.abort->store(true);
        }
    }
    for (Job& job : jobs_) {
        if (job.thread != nullptr) {
            if (::WaitForSingleObject(job.thread, kShutdownJoinMs) != WAIT_OBJECT_0) {
                log_format(L"deskfeed: 取数线程 %llu 未在 %u ms 内退出，放弃等待", job.id,
                           kShutdownJoinMs);
            }
            ::CloseHandle(job.thread);
            job.thread = nullptr;
        }
    }
    jobs_.clear();
    // 线程都收完了（或已经放弃等待）：停止接受新结果。队列对象本身由 shared_ptr 活着——
    // 万一还有线程没真正 return，它手里那份所有权会让队列与锁继续有效，直到它自己结束。
    // 绝不在还有线程可能碰它的时候销毁队列。
    if (outcomes_) {
        outcomes_->stop_accepting();
    }
    // 先撤任务栏那块：它是唯一会碰**别的进程**的那条路。收尾只**请求**一次停止（异步、不等待、
    // 不重试、绝不强杀）；再发一帧 closed 叫醒还挂着的负载去清理自己那部分显示。
    supervisor_.shutdown();
    if (frame_producer_.created()) {
        frame_producer_.publish_closed(wall_ms());
    }
    // 悬浮窗随后关：destroy() 会**同步**再调一次 save_preferences（补写欠着的锚点），
    // 所以 settings_ 必须活到它返回之后——Master 到析构才会释放 settings_，这里绝不能提前 reset。
    if (widget_created_) {
        widget_.destroy();
        widget_created_ = false;
    }
    binding_.clear();
    server_.destroy();
}

int Master::run() {
    if (!initialize_control()) {
        return 7;
    }

    // 先读磁盘上的调度配置：即使启动命令一个调度开关都不给，也要沿用保存的值；
    // 命令里显式给的 --poll-ms / --retry-ms / --timeout-ms 随后覆盖它。
    SourceSettings source = settings_ ? settings_->read_source() : SourceSettings{};
    ScheduleConfig config;
    config.poll_ms = schedule_clamp_poll(source.poll_ms).value;
    config.retry_ms = schedule_clamp_retry(source.retry_ms).value;
    config.timeout_ms = schedule_clamp_timeout(source.timeout_ms).value;
    schedule_ = config;
    scheduler_.set_config(schedule_);

    if (command_.kind == CommandKind::Apply) {
        ControlReply const reply = apply_command(command_, 0);
        if (reply != ControlReply::Accepted) {
            log_line(L"deskfeed: 首次启动的命令被拒绝，退出");
            server_.destroy();
            log_close();
            return reply == ControlReply::Rejected ? 6 : 2;
        }
    } else {
        if (!source.url.empty()) {
            UrlParts parts;
            std::wstring url_error;
            if (url_crack(source.url, parts, url_error)) {
                url_ = source.url;
                configured_ = true;
                health_.set_configured(true);
                scheduler_.reset(monotonic_ms());
                log_format(L"deskfeed: 用保存的地址独立启动：%s", redact(url_).c_str());
            } else {
                configured_ = false;
                health_.set_configured(false);
                log_line(L"deskfeed: 保存的地址不合法：" + url_error);
            }
        } else {
            configured_ = false;
            health_.set_configured(false);
            log_line(L"deskfeed: 没有保存的地址；显示“未配置 URL”");
        }
    }

    // 到这里为止首份状态**已经被接受**了：
    //   * `--url` 命令：apply_command 返回 Accepted（地址校验过、绑定准备过、配置落盘过）；
    //   * 无参数启动：保存的状态读出来了，地址校验过，或明确判定为“未配置 URL”。
    // 只有从这一刻起才建显示面（显示邮箱、悬浮窗、任务栏编排）——首份命令被拒绝时，上面那条
    // 分支已经带着退出码走掉了，绝不留下一块按生产命名建好的邮箱或一次注入副作用。
    if (!start_display_face()) {
        server_.destroy();
        log_close();
        return 7;
    }

    // 显示面一就位就先发一帧（force）：读端一挂上就该有东西可读，不必等第一次取数回来。
    // 这也是“任务栏组件被打开/重新注入时立刻给一帧缓存”的同一条路径。
    publish_display_outputs(true);

    for (;;) {
        if (quit_requested_) {
            break;
        }
        // 状态推进与发帧都在这一处（模态循环期间由窗口过程/心跳再调它一次）。
        pump(false);
        if (quit_requested_) {
            break;
        }

        // 等待时间：时钟域不能混。调度与超时用单调时钟，到期是快照里的绝对 Unix 时间。
        long long const now = monotonic_ms();
        bool const at_valve = jobs_.size() >= kMaxFetchJobs;
        long long wait = 60000;
        if (active_job_id_ != 0) {
            wait = fetch_deadline_ms_ - now;
        } else if (at_valve) {
            wait = 1000;  // 作业记录已到硬上限：等某个被放弃的取数收手
        } else if (configured_) {
            wait = scheduler_.due_ms() - now;
        }
        long long const expiry = health_.next_expiry_ms();
        if (expiry >= 0) {
            long long const expiry_wait = expiry - wall_ms();
            if (expiry_wait < wait) {
                wait = expiry_wait;
            }
        }
        if (wait < 0) {
            wait = at_valve ? 1000 : 0;
        }
        if (wait > 60000) {
            wait = 60000;
        }
        if (wait > kHeartbeatMs) {
            // 心跳最快也就 1 s 一次：再长的等待没有意义，只会让模态循环之后的状态推进变钝。
            wait = kHeartbeatMs;
        }

        HANDLE handles[kMaxFetchJobs]{};
        DWORD count = 0;
        for (Job const& job : jobs_) {
            if (count < kMaxFetchJobs) {
                handles[count++] = job.thread;
            }
        }
        DWORD const waited = ::MsgWaitForMultipleObjects(count, count > 0 ? handles : nullptr, FALSE,
                                                         static_cast<DWORD>(wait), QS_ALLINPUT);
        if (waited == WAIT_FAILED) {
            log_format(L"deskfeed: MsgWaitForMultipleObjects 失败：%s", last_error_text().c_str());
            break;
        }

        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                quit_requested_ = true;
                break;
            }
            // 主进程自己的 WM_APP / 心跳：与窗口过程走同一个处理函数（那里也有一条入口，
            // 因为嵌套模态循环只会把消息派发到窗口过程，不会回到这里）。
            if (on_window_message(message.message, message.wParam, message.lParam)) {
                continue;
            }
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }

        // 消息先处理完再回收线程：取数结果正是通过消息投递的，先回收会把结果误判成迟到。
        reap_jobs();

        // 消息处理阶段（取数结果、控制命令）改过模型的话，在本轮结束前再落地一次。
        pump(false);
    }

    shutdown();
    log_line(L"deskfeed: gone");
    log_close();
    return 0;
}

void Master::drain_outcomes() {
    if (!outcomes_) {
        return;
    }
    for (;;) {
        std::unique_ptr<FetchOutcome> outcome = outcomes_->pop();
        if (!outcome) {
            return;
        }
        on_fetch_done(std::move(outcome));
    }
}

bool Master::on_window_message(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case kMsgFetchDone:
            // 唤醒消息**不带指针**：结果在队列里，出队处理。带指针的投递会在嵌套模态循环里
            // 被 DefWindowProc 静默吞掉（结果丢失 + 内存泄漏），所以这里只认队列。
            (void)lparam;
            drain_outcomes();
            return true;
        case kMsgBindingExit:
            on_binding_exit(static_cast<std::uint64_t>(wparam));
            return true;
        case kMsgQuit: {
            // wParam = 投递时记下的绑定代次（0 = 无条件退出）。判定与动作分两处，所以真正的
            // 核验发生在 consume_quit 里——这里只负责“要不要把它推迟消费”。
            std::uint64_t const wanted = static_cast<std::uint64_t>(wparam);
            if (defer_quits_ && pending_quits_.size() < kMaxDeferredQuits) {
                pending_quits_.push_back(
                    PendingQuit{wanted, monotonic_ms() + static_cast<long long>(defer_quit_ms_)});
                log_format(L"quit: 测试注入点延迟消费退出消息 %lu ms（generation %llu，队列 %zu）",
                           defer_quit_ms_, wanted, pending_quits_.size());
                return true;
            }
            consume_quit(wanted);
            return true;
        }
        case kMsgRefreshRequested:
            // 任务栏那块被双击/点「立即刷新读数」：对**当前**地址立刻取一次数。
            // 不追加任何查询参数、不改调度配置、不动绑定与地址——一次性的取数请求。
            // 已经有一次有效取数在跑时不做第二次：刷新不是“多开一条并行请求”。
            if (configured_ && active_job_id_ == 0) {
                log_line(L"refresh: 任务栏请求立即刷新，对当前地址再取一次");
                start_job();
            } else if (!configured_) {
                log_line(L"refresh: 没有配置地址，刷新请求不产生取数");
            } else {
                log_line(L"refresh: 已经有一次取数在跑，刷新请求合并到这一次");
            }
            return true;
        case kMsgShowRequested:
            // 任务栏那块被双击：把悬浮窗叫出来。窗口本体由桌面侧负责，主进程这边要做的
            // 是**取消用户的隐藏意图**（并落盘），窗口才会真的出现；`--headless` 下没有
            // 悬浮窗可显示，请求到此为止（不建窗口、不注入）。
            show_requested_ = true;
            return true;
        default:
            break;
    }
    if (message == WM_TIMER) {
        // 心跳：模态循环期间主循环跑不到，靠它把状态机继续推下去。空转时 pump 什么也不做。
        pump(true);
        return true;
    }
    return false;
}

void Master::pump(bool from_modal_dispatch) {
    if (pumping_) {
        // 嵌套模态循环里再派发到同一条消息：绝不递归进状态机。
        (void)from_modal_dispatch;
        return;
    }
    pumping_ = true;

    long long const now = monotonic_ms();
    // 被测试注入点延迟的退出消息在这里到期消费：到期之前不会置 closing_/quit_requested_，
    // 所以“退出已投递、尚未生效”的那段窗口是确定可用的。
    drain_pending_quits(now);
    health_.tick(wall_ms());

    // 整次取数的墙钟预算由主进程守：到点就当场宣布失败并放弃这次取数，
    // 不等底层调用自己超时（慢速滴流也拖不过这个截止）。
    if (active_job_id_ != 0 && now >= fetch_deadline_ms_) {
        if (Job* job = find_job(active_job_id_)) {
            job->active = false;
            job->timed_out = true;
            if (job->abort) {
                job->abort->store(true);
            }
        }
        active_job_id_ = 0;
        health_.apply_failure(FailureKind::Timeout,
                              "超过 " + std::to_string(schedule_.timeout_ms) + " ms 整次取数预算",
                              wall_ms());
        scheduler_.on_result(false, now);
        log_format(L"fetch: 超过 %lld ms 墙钟预算，放弃这一次", schedule_.timeout_ms);
    }

    // 工作线程报告“结果没能入队”（队列满 / 已经停止接受 / 线程内异常）：立刻把这次取数按
    // 内部失败收尾并释放名额，不干等墙钟预算。线程本身会自己结束，reap_jobs 随后回收它。
    if (active_job_id_ != 0) {
        if (Job* job = find_job(active_job_id_)) {
            if (job->finished_without_result && job->finished_without_result->load()) {
                job->active = false;
                job->timed_out = true;  // 结果不会再来：当已放弃处理
                active_job_id_ = 0;
                health_.apply_failure(FailureKind::Internal, "取数结果没能交回主进程", wall_ms());
                scheduler_.on_result(false, now);
                log_format(L"fetch: job %llu 的结果没能入队，按内部失败收尾", job->id);
            }
        }
    }

    std::size_t live_jobs = 0;
    for (Job const& job : jobs_) {
        if (job.active) {
            ++live_jobs;
        }
    }

    if (configured_ && active_job_id_ == 0 && scheduler_.due_ms() <= now &&
        live_jobs < kMaxLiveFetches && jobs_.size() < kMaxFetchJobs) {
        start_job();
    }

    if (show_requested_) {
        show_requested_ = false;
        // 任务栏那块的双击/「打开桌面悬浮窗」：请悬浮窗显示自己。它会清掉用户的隐藏意图并
        // 落盘（经 save_preferences 回调），然后按全屏让位等规则决定是否真的露出来。
        // 这里**不**顺便取数：显示是显示，取数是取数；`--hidden` 只是本次运行的开关，
        // 窗口在 force_hidden 下不会露出来，也绝不会因此改掉用户保存的 hidden。
        if (widget_created_) {
            widget_.show_from_request();
        } else {
            log_line(L"show: 任务栏请求打开悬浮窗，但本次运行没有悬浮窗（--headless）");
        }
    }

    // 任务栏编排：非阻塞地回收已经退出的注入器，再按当前目标状态决定下一步动作。
    // 它绝不阻塞、绝不强杀；一次动作还在跑时只把意图记成令牌。
    supervisor_.tick(now);

    // 在阻塞等待之前发布，位置在**本轮全部状态转换之后**：到期 tick、整次取数的墙钟截止，
    // 以及 start_job() 自己（CreateThread 失败会当场 apply_failure(Internal)）。再往下主循环
    // 就要睡到下一个轮询点；只在循环末尾发布会让显示面在“模型已经变了”之后继续显示旧状态，
    // 直到下一次取数或别的消息把它叫醒。到期由主进程按绝对时间独立判定，失败一发生就进入
    // 整体失效——这两个时刻起就必须是读端看到它们的时刻。revision 没变时这里是空操作。
    publish_display_outputs(false);

    pumping_ = false;
}

std::string Master::inspect_json() const {
    bool url_removed = false;
    std::wstring const runtime_redacted = url_redact(url_, &url_removed);
    bool disk_removed = false;
    std::wstring disk_url;
    if (settings_) {
        disk_url = settings_->read_source().url;
    }
    std::wstring const disk_redacted = url_redact(disk_url, &disk_removed);

    View const view = health_.view();
    long long const now = monotonic_ms();
    long long const next_fetch =
        configured_ ? std::max<long long>(0, scheduler_.due_ms() - now) : 0;

    std::string out = "{";
    out += "\"instance\":" + json_wstring(scope_);
    out += ",\"masterPid\":" + std::to_string(::GetCurrentProcessId());
    out += ",\"runtimeUrl\":" + json_wstring(runtime_redacted);
    out += ",\"runtimeUrlRedacted\":" + std::string(url_removed ? "true" : "false");
    out += ",\"diskUrl\":" + json_wstring(disk_redacted);
    out += ",\"configured\":" + std::string(configured_ ? "true" : "false");
    if (binding_.active()) {
        out += ",\"boundPid\":" + std::to_string(binding_.pid());
    } else {
        out += ",\"boundPid\":null";
    }
    out += ",\"sourceGeneration\":" + std::to_string(source_generation_);
    out += ",\"bindingGeneration\":" + std::to_string(binding_generation_);
    out += ",\"schedule\":{\"pollMs\":" + std::to_string(schedule_.poll_ms) +
           ",\"retryMs\":" + std::to_string(schedule_.retry_ms) +
           ",\"timeoutMs\":" + std::to_string(schedule_.timeout_ms) + "}";
    out += ",\"nextFetchInMs\":" + std::to_string(next_fetch);

    char const* kind = "waiting";
    if (view.kind == ViewKind::Content) {
        kind = "content";
    } else if (view.kind == ViewKind::Error) {
        kind = "error";
    }
    out += ",\"view\":{\"kind\":\"";
    out += kind;
    out += "\",\"alarm\":" + std::string(view.alarm ? "true" : "false");
    out += ",\"failure\":" + json_string(failure_kind_name(view.failure));
    out += ",\"reason\":" + json_string(view.reason);
    out += ",\"items\":[";
    for (std::size_t index = 0; index < view.items.size(); ++index) {
        ViewItem const& item = view.items[index];
        if (index > 0) {
            out += ",";
        }
        out += "{\"id\":" + json_string(item.id) + ",\"label\":" + json_string(item.label) +
               ",\"value\":" + json_string(item.value) +
               ",\"state\":" + json_string(item_state_name(item.state)) +
               ",\"detail\":" + json_string(item.detail) + "}";
    }
    out += "]}";

    out += ",\"hasSnapshot\":" + std::string(health_.has_snapshot() ? "true" : "false");
    out += ",\"expired\":" + std::string(health_.expired() ? "true" : "false");
    out += ",\"receivedAtMs\":";
    out += health_.has_snapshot() ? std::to_string(health_.received_at_ms()) : std::string("null");
    out += ",\"expiresAtMs\":";
    if (health_.has_snapshot() && health_.snapshot().has_expires_at) {
        out += std::to_string(health_.snapshot().expires_at_ms);
    } else {
        out += "null";
    }
    out += "}";
    return out;
}

std::wstring default_class_name(std::wstring const& scope) {
    return scoped_name(kControlWindowClassBase, scope);
}

std::wstring master_mutex_name(std::wstring const& scope) {
    return scoped_name(L"Local\\DeskFeed-master", scope);
}

// 带 `--wait-exit-ms` 的退出转发：**只有**真正拿到目标实例进程的 SYNCHRONIZE 句柄、并在确认
// 返回 `Accepted` 之后等到那个句柄真的触发，才返回 0。
//
// 这里刻意 fail closed：拿不到句柄、控制窗口不在、或者窗口属主在发送前发生变化，都返回 5
// （不确定）并且**不发送 IPC**。绝不退化成“只等确认”的旧语义——那会让调用方把一次 ACK 当成
// “实例已经退出”，从而在旧实例还活着（或正在收尾）时误判为可以另起一个。
//
// 句柄必须是**发送前**从控制窗口属主 PID 上拿到的那个：确认返回之后实例可能立刻销毁窗口，
// 那时再按 PID 重新打开就是在猜“还是同一个实例”。发送前会再核一次属主 PID 与句柄存活，
// 避免 HWND 被复用后把请求发给另一个实例。
int forward_quit_and_wait(std::vector<std::wstring> const& args, std::wstring const& class_name,
                          unsigned long wait_exit_ms, bool allow_test_failpoints) {
    ControlRequest request;
    request.op = ControlOp::Command;
    request.sender_pid = ::GetCurrentProcessId();
    request.args = args;

    HWND const window = control_find_window(class_name);
    if (window == nullptr) {
        // 调用方已经确认互斥体被占（有实例），但控制窗口不在：实例正在收尾，拿不到真实 PID。
        // 不能冒充“实例已退出”，也不去猜一个 PID。
        console_write_stderr(L"deskfeed: 找不到实例的控制窗口，无法确认它是否真的退出（不确定）\n");
        return 5;
    }
    DWORD owner_pid = 0;
    ::GetWindowThreadProcessId(window, &owner_pid);
    if (owner_pid == 0 || owner_pid == ::GetCurrentProcessId()) {
        console_write_stderr(L"deskfeed: 控制窗口的属主进程不合法，无法确认目标实例（不确定）\n");
        return 5;
    }
    // 测试注入点：模拟 OpenProcess 失败，用来确定性验证“拿不到句柄就不发 IPC、返回 5”这条
    // fail-closed 路径。只在隔离实例（scope 非空）里可能生效。
    bool const simulate_no_handle =
        allow_test_failpoints && read_test_environment(L"DESKFEED_TEST_FAIL_WAIT_HANDLE") == L"1";
    HANDLE const master_process =
        simulate_no_handle ? nullptr : ::OpenProcess(SYNCHRONIZE, FALSE, owner_pid);
    if (master_process == nullptr) {
        // 拿不到一个能等它退出的句柄 = 没有任何“实例是否真的退出”的证据。fail closed：不发 IPC。
        console_write_stderr(L"deskfeed: 拿不到实例进程的同步句柄，无法确认它是否真的退出（不确定）\n");
        return 5;
    }
    // 发送前的第二次身份核验：窗口还在、属主还是同一个 PID、句柄还没触发。
    DWORD recheck_pid = 0;
    if (::IsWindow(window)) {
        ::GetWindowThreadProcessId(window, &recheck_pid);
    }
    if (recheck_pid != owner_pid || ::WaitForSingleObject(master_process, 0) == WAIT_OBJECT_0) {
        ::CloseHandle(master_process);
        console_write_stderr(L"deskfeed: 控制窗口的属主在发送前发生变化，无法确认目标实例（不确定）\n");
        return 5;
    }

    ControlReply reply = ControlReply::BadRequest;
    std::string error;
    if (!control_send_window(window, request, kControlSendTimeoutMs, reply, error)) {
        ::CloseHandle(master_process);
        console_write_stderr(L"deskfeed: 没有收到实例确认（" + wide_from_utf8(error) +
                             L"）；这不代表命令没有执行\n");
        return 5;
    }
    int result = 0;
    switch (reply) {
        case ControlReply::Accepted: {
            // 只有这个**发送前**捕获的句柄真的触发，才算“实例已退出”。
            DWORD const waited = ::WaitForSingleObject(master_process, wait_exit_ms);
            if (waited != WAIT_OBJECT_0) {
                // 到点还没退：仍然属于“未确认退出”，不能当成没执行，更不许补杀或重开。
                console_write_stderr(L"deskfeed: 等待实例退出超时；命令已被接受，实例可能稍后退出\n");
                result = 5;
            }
            break;
        }
        case ControlReply::NotBound:
            result = 0;  // 条件不匹配：成功返回、什么都不做（安全 no-op），不等待
            break;
        case ControlReply::Rejected:
        case ControlReply::BadRequest:
            console_write_stderr(L"deskfeed: 运行中的实例拒绝了这条命令\n");
            result = 6;
            break;
    }
    ::CloseHandle(master_process);
    return result;
}

// 把一条命令转发给运行中的实例。
//
// `wait_exit_ms == 0`（缺省）= 旧语义：只等 IPC 确认，确认不等于实例已经退出。这条路径与
// 之前完全一致，私有调用方不写新开关时行为不变。
// `wait_exit_ms != 0` 走上面的 fail-closed 等待路径。
int forward_command(std::vector<std::wstring> const& args, std::wstring const& class_name,
                    unsigned long wait_exit_ms, bool allow_test_failpoints) {
    if (wait_exit_ms > 0) {
        return forward_quit_and_wait(args, class_name, wait_exit_ms, allow_test_failpoints);
    }

    ControlRequest request;
    request.op = ControlOp::Command;
    request.sender_pid = ::GetCurrentProcessId();
    request.args = args;

    ControlReply reply = ControlReply::BadRequest;
    std::string error;
    if (!control_send(class_name, request, kControlSendTimeoutMs, reply, error)) {
        console_write_stderr(L"deskfeed: 没有收到实例确认（" + wide_from_utf8(error) +
                             L"）；这不代表命令没有执行\n");
        return 5;
    }
    switch (reply) {
        case ControlReply::Accepted:
        case ControlReply::NotBound:
            return 0;
        case ControlReply::Rejected:
        case ControlReply::BadRequest:
            console_write_stderr(L"deskfeed: 运行中的实例拒绝了这条命令\n");
            return 6;
    }
    return 6;
}

int inspect_instance(std::wstring const& class_name, std::wstring const& scope) {
    unsigned long const self = ::GetCurrentProcessId();
    ControlRequest request;
    request.op = ControlOp::Inspect;
    request.sender_pid = self;

    ControlReply reply = ControlReply::BadRequest;
    std::string error;
    if (!control_send(class_name, request, kControlSendTimeoutMs, reply, error)) {
        console_write_stderr(L"deskfeed: 没有运行中的实例（" + wide_from_utf8(error) + L"）\n");
        return 5;
    }
    if (reply != ControlReply::Accepted) {
        console_write_stderr(L"deskfeed: 实例拒绝了查询\n");
        return 6;
    }

    std::wstring const path = deskfeed_inspect_path(scope, self);
    std::string bytes;
    bool exists = false;
    std::wstring read_error;
    if (!read_file_bytes(path, bytes, exists, read_error) || !exists) {
        console_write_stderr(L"deskfeed: 读不到查询结果 " + path + L"\n");
        return 7;
    }
    console_write_stdout(bytes);
    console_write_stdout("\n");
    delete_file(path);
    return 0;
}

}  // namespace

int master_main(std::vector<std::wstring> const& args) {
    Command command;
    std::wstring parse_error;
    if (!command_parse(args, command, parse_error)) {
        console_write_stderr(L"deskfeed: " + parse_error + L"\n\n" +
                             wide_from_utf8(command_usage()));
        return 2;
    }
    if (command.kind == CommandKind::Help) {
        console_write_stdout(command_usage());
        return 0;
    }

    // 实例命名空间：显式 --scope 优先，其次环境变量；两者都要过白名单。
    std::wstring const scope = command.scope.empty() ? instance_suffix() : command.scope;
    std::wstring scope_error;
    if (!scope_validate(scope, scope_error)) {
        console_write_stderr(L"deskfeed: " + scope_error + L"\n");
        return 2;
    }

    std::wstring const class_name = default_class_name(scope);
    std::wstring const mutex_name = master_mutex_name(scope);

    UserSecurity security;
    if (!security.build()) {
        // 建不出只允许当前用户的安全描述符就退回系统默认；对象名仍带用户哈希。
        console_write_stderr(L"deskfeed: 警告：没能建立用户范围的安全描述符，改用系统默认\n");
    }

    InstanceMutex mutex;
    bool already_running = false;
    std::string mutex_error;
    if (!mutex.acquire(mutex_name, &security, already_running, mutex_error)) {
        console_write_stderr(L"deskfeed: " + wide_from_utf8(mutex_error) + L"\n");
        return 7;
    }

    switch (command.kind) {
        case CommandKind::QuitIfBound:
        case CommandKind::Quit:
            if (!already_running) {
                return 0;  // 条件退出没有实例：成功返回，不启动任何东西
            }
            {
                int const result = forward_command(args, class_name, command.wait_exit_ms,
                                                   !scope.empty());
                // 收尾窗口：主实例正在退出，控制窗口已经销毁、互斥体还没释放（收尾顺序就是
                // 先关窗口再退进程）。这时“没有可退的实例”与契约里的“没有实例”是同一件事，
                // 按成功返回，而不是把一个正在消失的实例报成失败。
                //
                // 判据必须**同时**满足：投递失败，且控制窗口确实已经不在了。窗口还在却超时，
                // 那是真的没应答——契约明确说这种情况是“不确定”，绝不能当成成功。
                //
                // 但调用方显式要求“等到实例真的退出”时不套用这条启发式：窗口不在不代表进程
                // 已经退出，等待超时就是“未确认退出”，必须如实返回 5，不能假报成功。
                if (command.wait_exit_ms == 0 && result != 0 && !control_window_exists(class_name)) {
                    log_line(L"quit: 控制窗口已经不在（实例正在收尾）；按没有实例处理");
                    return 0;
                }
                return result;
            }
        case CommandKind::Inspect:
            if (!already_running) {
                console_write_stderr(L"deskfeed: 没有运行中的实例\n");
                return 5;
            }
            return inspect_instance(class_name, scope);
        case CommandKind::Apply:
            if (already_running) {
                // 接管命令不带“等实例退出”的语义（命令行解析也不允许），永远走 ACK 语义。
                return forward_command(args, class_name, 0, !scope.empty());
            }
            break;
        case CommandKind::Start:
            if (already_running) {
                return 0;  // 无参数：不接管，也不改变地址与绑定
            }
            break;
        case CommandKind::Help:
            return 0;
    }

    Master master(command, scope);
    return master.run();
}

}  // namespace deskfeed
