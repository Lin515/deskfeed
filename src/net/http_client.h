#pragma once

#include "core/snapshot.h"  // kMaxBodyBytes：正文上限只有一处定义

#include <atomic>
#include <cstddef>
#include <string>

// 唯一的外部取数入口：只发 GET，只接纳 200 + UTF-8 JSON。
//
// 规则（见 docs/protocol.md 的“HTTP 规则与调度”，以及 docs/http.md 的实现说明）：
//   * 一个墙钟预算管住整次取数，含重定向与正文读取；慢速滴流不会无限拖住线程；
//     这一层用异步 WinHTTP + 自己的截止等待实现真正的整次取消（见 .cpp 顶部说明）；
//   * 2xx 之外都不算快照：204 / 304 不能代替一次清空；清空必须以 200 + items: [] 表达；
//   * 重定向自己跟（最多 5 跳），禁止 HTTPS 降级到 HTTP，且不改写保存的配置地址；
//   * 正文上限 1 MiB，边读边判，不等读完再拒绝；
//   * 不使用系统代理（回环地址走代理没有意义，且无网络时自动代理探测会白等）。
namespace deskfeed {

inline constexpr int kMaxRedirects = 5;

enum class HttpFailure {
    None,
    InvalidUrl,  // 地址本身不合法或协议不对
    Connect,     // 连不上、名字解析不了、连接中断、TLS 握手失败
    Timeout,     // 超过墙钟预算
    Send,        // 请求发不出去
    Receive,     // 收响应出错
    Redirect,    // 重定向超次数或试图降级
    Status,      // 上游返回非 200
    TooLarge,    // 正文超过 1 MiB
    Empty,       // 正文是空的
    Aborted,     // 主进程要求停手（收尾中）
};

char const* http_failure_name(HttpFailure failure);

struct HttpLimits {
    std::size_t max_body_bytes = kMaxBodyBytes;
    int max_redirects = kMaxRedirects;
    long long timeout_ms = 8000;  // 整次取数的墙钟预算
};

struct HttpResult {
    bool ok = false;
    long status = 0;  // 最终响应的状态码；没到响应阶段时为 0
    std::string body;
    HttpFailure failure = HttpFailure::None;
    std::string error;  // 面向排障的一句话（UTF-8）
};

// 同步取数（内部是异步 WinHTTP 状态机）。调用方负责把它放在独立的工作线程上。
// 返回时保证已经在墙钟预算附近结束：到点或被 abort 取消时，函数关掉在途的请求句柄
// （异步操作未完成时关闭相关句柄是文档化的取消方式），不会把线程留在网络等待里。
// abort 非空时大约每 50 ms 检查一次，换源/退出的取消足够及时。
HttpResult http_get(std::wstring const& url, HttpLimits const& limits,
                    std::atomic<bool> const* abort = nullptr);

}  // namespace deskfeed
