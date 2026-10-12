#pragma once

#include <string>

// `deskfeed-probe.exe` 的**离线量具**：把一份文件变成读数文本，不起窗口、不起线程、不碰 XAML、
// 不加载负载 DLL，也不碰 explorer。
//
// 三种输入刻意分开，因为它们的地位完全不同：
//   * `--reading <snapshot.json>` / `--reading-rows <snapshot.json>`：**严格的通用快照 v1**。
//     走 core 的 `snapshot_validate` 与 health 状态机（含 `expiresAtMs` 的真实 Unix 毫秒判定），
//     再由纯格式化器排版。这是公开协议的量具。
//   * `--frame-reading <frame.json>`：**只用于诊断**的一份显示帧 JSON
//     （`kind` / `alarm` / `failure` / `reason` / `items[]`）。它不是第二套生产端协议：正式生产端
//     只产出快照 v1，这一路是为了在没有主进程、没有邮箱的情况下把 Waiting/Error/整体 alarm 这些
//     显示分支画出来。
//
// 三路共用 `display/reading.h` 的同一批纯函数，所以量具与窗口/任务栏不可能给出第二种排版。
namespace deskfeed::probe {

enum class OfflineMode {
    Reading,      // --reading：折行后的行 + tip + 展开（与桌面悬浮窗的转储同一份排版）
    ReadingRows,  // --reading-rows：来源数、每行 prefix/value/error 与实际排法
    FrameReading, // --frame-reading：诊断帧 JSON 的同一份排版
};

// 跑一次离线量具。成功返回 true 并返回退出码 0；失败返回 false，`exit_code` 已定好，原因已经
// 写到标准错误（这一层不打印别的东西，`--reading` 的输出要能逐字节比对）。
bool run_offline(OfflineMode mode, std::wstring const& path, long long refresh_ms, int& exit_code);

// 标准输出的写法：控制台上走宽字符，被重定向时走 UTF-8（读数里的 ℃、— 两种情形都要能读出来）。
void write_stdout(std::wstring const& text);
void write_stderr(std::wstring const& text);

}  // namespace deskfeed::probe
