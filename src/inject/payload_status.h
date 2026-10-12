#pragma once

#include <cstdint>
#include <string>

#include "inject/remote.h"
#include "inject/status_pod.h"

// 负载的生命周期状态：`--status` 与 `--load` 都靠它区分
// "模块映射着" / "功能真的在跑（且跑在**这个** scope 上）" / "线程结束了但没能安全退休"。
//
// 只读：除了在目标进程里起一次只写 POD、不做任何状态转移的远程线程，本模块不改变任何东西。
namespace deskfeed::inject {

enum class PayloadState {
    NotMapped = 0,     // 模块表里没有这个负载
    MappedUnknownAbi,  // 映射着，但报不出状态（DLL 没有 DeskFeedGetStatus / ABI 版本不认）
    MappedUnverifiable,// 映射着，但这**一次**查询的结论不可信：远程调用超时/未创建、读回失败、
                       // 磁盘与远程镜像身份不一致、或 POD 结构本身不可信。绝不能当成 Inactive
    MappedInactive,    // 映射着但不能证明在工作：线程已结束、没挂上邮箱、生产者已死、scope 不对
    MappedActive,      // 可证明在工作：线程活着 + 挂上本 scope 邮箱 + 生产者活着且身份一致
    MappedRetained,    // 有悬挂，或线程结束却报告 unload_safe=0：绝不可强卸
};

char const* payload_state_name(PayloadState state);

// 一个 scope 的显示邮箱帧头里记的生产者身份（只读探询）。
struct MailboxProducer {
    bool present = false;
    unsigned long pid = 0;
    std::uint64_t start_time = 0;
    std::uint32_t generation = 0;
    // 帧头带 `kFrameFlagClosed`：生产者已经正常收尾。此时负载**不该**继续显示读数，所以它不能被
    // 算成"活跃"，哪怕生产者进程还没退出、负载线程还活着。
    bool closed = false;
};

struct PayloadStatus {
    PayloadState state = PayloadState::NotMapped;
    bool mapped = false;
    ModuleEntry module;
    bool have_pod = false;
    PayloadStatusPod pod;
    bool mailbox_present = false;
    bool mailbox_producer_closed = false;
    unsigned long mailbox_producer_pid = 0;
    std::uint64_t mailbox_producer_start = 0;
    bool mailbox_producer_alive = false;
    bool producer_consistent = false;  // POD 报的生产者 == 帧头里的生产者
    bool scope_matches = false;        // POD 报的 scope == 命令行 scope
    // 这一次查询的结论是否**可信**：只有真的读到并信任了一份 POD（Inactive/Active/Retained）
    // 才为 true。`MappedUnknownAbi` / `MappedUnverifiable` 一律 false —— 任何改变目标状态的
    // 动作（Stop / eject / 重装）都必须先要求它为 true。
    bool conclusive = false;
    std::wstring detail;               // 一行排障说明（进日志）

    bool active_proven() const { return state == PayloadState::MappedActive; }
};

// 门禁（纯函数，可离线钉住）：
//   * 只有"结论可信 + 状态为 Inactive"才允许"先 Stop 再重装"；
//   * 只有"结论可信 + 状态为 Inactive/Active/Retained"才允许对本目标发起 Stop；
// 不可信（超时、ABI 不认、POD 不可信、身份不一致）一律不能进入任何 Stop/eject 分支。
inline bool state_allows_reload_stop(PayloadStatus const& status) {
    return status.conclusive && status.state == PayloadState::MappedInactive;
}

inline bool state_allows_stop(PayloadStatus const& status) {
    return status.conclusive && (status.state == PayloadState::MappedInactive ||
                                 status.state == PayloadState::MappedActive ||
                                 status.state == PayloadState::MappedRetained);
}

// 只读地把 scope 的显示邮箱帧头读一遍。共享内存不存在时返回 true 且 `present=false`；
// 只有在别的原因（名字非法、映射打开失败、帧头一直不稳定/不合法）下才返回 false。
bool probe_mailbox_producer(std::wstring const& scope, MailboxProducer& out, std::wstring& error);

// 组合查询：模块表 + 磁盘导出的 `DeskFeedGetStatus` + 帧头生产者身份。
bool query_payload_status(unsigned long process_id, std::wstring const& dll_path,
                          std::wstring const& scope, PayloadStatus& out, std::wstring& error);

}  // namespace deskfeed::inject
