#pragma once

#include <windows.h>

#include <string>

namespace deskfeed::probe {

// 自测宿主（`deskfeed-probe.exe`）的输入。两种模式**互斥**，不许同时开：
//   * 结构自测（默认）：XAML 诊断 → 一次走查 → 在宿主线程上解析并应用 → 拆除 → 归还引用。
//   * `--feed`：把宿主自己的岛登记成负载的"托盘目标"，然后跑**生产入口** `DeskFeedStart`，
//     让负载自己从探针的 mock 邮箱读帧、并把块更新到宿主窗口上（真实 hook 编组）。
struct HostOptions {
    std::wstring dll_path;
    std::wstring scope = L"probe";
    unsigned cores = 1;      // 本进程自己起几个 XAML 核心（引用计数只有在 >1 时才分得清）
    unsigned seconds = 0;    // 解析/应用之后的观测窗口
    bool feed = false;       // 生产入口 + mock 邮箱
    bool dump_only = false;  // 只读树、不插入
    bool hook = false;       // reg hook 自检（装钩前/后各读一次）
    bool teardown = false;   // 走负载的真实拆除路径
    bool balance_check = false;  // 再调一次 ProbeStop，验证不多还也不少还
    bool third_party = false;    // 装第三方钩子并验收尾顺序
    bool mutation_early_done = false;  // 对抗 failpoint：把 done 提前到 CallNext 之前
    unsigned install_delay_ms = 0;     // 安装竞态窗口（发布句柄之前人为延迟）
    unsigned hang_ui_ms = 0;           // UI 线程停止取消息若干毫秒
    unsigned slow_apply_ms = 0;        // 让 UI 线程上的 apply 慢若干毫秒
};

// 跑一次自测宿主。返回 0 表示按预期完成（对抗用例里"观察到对抗症状"也算按预期）。
int run_host(HostOptions const& options);

// 收尾判据：逐次 `FreeLibrary` 直到失败，打印
// `payload module references at the end: N`（健康路径必须是 1：那 1 次是探针自己的 LoadLibrary）。
void report_module_references(HMODULE module);

}  // namespace deskfeed::probe
