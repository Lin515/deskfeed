#pragma once

#include <string>

// 任务栏那块上两个交互（双击、右键菜单项）的**动作工作线程**。
//
// 为什么要有它：任务栏线程上不能阻塞。一次 `SendMessageTimeout` 最长等 1.5 秒，放在 UI 线程上
// 就是把任务栏冻住。所以交互处理器只做一件不阻塞的事——置一个合并标志、唤醒这个常驻工作线程，
// 真正的发送在它上面跑。
//
// 生命周期同样受 G2 的收尾门禁约束：**只有线程句柄真的触发（线程已终止）才允许报告"可以卸载"**。
// `running=false` 但仍在模块里返回的线程一律算"未退休"，模块必须保留。
namespace deskfeed::payload {

bool action_worker_start(std::wstring const& scope, std::wstring& error);

// 合并：已经有同类待发请求时不再排第二个（连点两次双击只发一次取数请求）。
void action_worker_post_refresh();
void action_worker_post_show_widget();

// 收尾。返回 true 表示工作线程已经终止（模块可以卸载）；false 表示有界时间内没退出，
// 调用方必须保留模块、不得报告可卸载。
bool action_worker_stop(unsigned timeout_ms);

bool action_worker_running();

#ifdef DESKFEED_NATIVE_TESTS
// **测试专用**（只在 `DESKFEED_NATIVE_TESTS` 的用例目标里编译，成品 DLL 里没有这个符号）：
// 让工作线程每一轮真正醒来之后先停住若干毫秒，用来确定性地制造"stop 在期限内等不到线程句柄"。
// 线程是真的没退出，重试路径随后必须能真的把它等回来。
void action_worker_probe_set_test_sleep_ms(unsigned ms);
#endif

}  // namespace deskfeed::payload
