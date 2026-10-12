#pragma once

// 回收门（retirement gates）：`unload_safe` 的**唯一**推导处。
//
// 为什么单独一个文件：结论的正确性不该只在"负载线程恰好怎么走"的时候才被检验。门位集合与
// "全过 && 没崩"这条与运算在这里，负载实现与离线用例用的是**同一份代码**，所以用例里咬住的
// 就是生产路径。
//
// 纪律：
//   * 每个门位都是**某条现成 API 的真实结果**（见 host.cpp 的 retire_gates），不发明不可证的位。
//   * 门位只可能由 false 变 true；失败**粘性**（`crashed` 同理）。没有任何一步能在末尾把已经记下
//     的失败改写成成功——`derived_unload_safe()` 是纯函数，不看"谁最后调用"。
//   * 新的一代（`DeskFeedStart`）才允许把这些位全部清回 false。
#include <atomic>
#include <cstdint>

namespace deskfeed::payload {

struct RetirementGates {
    // `action_worker_stop` 真的等到线程句柄触发
    std::atomic<bool> worker{false};
    // `taskbar_xaml_detach` 的完整结果（它内部已经 AND 了菜单/悬停/摘块/钩子静止证明）
    std::atomic<bool> xaml{false};
    // `xaml_tap_stop` 的真实证据：诊断站点归还 + SetSite 排空到零
    std::atomic<bool> tap{false};
    // `reg_hook_restore`（已接受残余窗口；恢复失败只影响它自己）
    std::atomic<bool> iat{false};
    // 邮箱消费者状态已经消失（identity 快照清空、mailbox_attached=false）
    std::atomic<bool> consumer{false};
    // 负载线程已经走完模块侧全部工作，只剩 `ret`
    std::atomic<bool> thread_done{false};
    // C++/SEH 故障，粘性
    std::atomic<bool> crashed{false};

    bool all_proven() const {
        return worker.load() && xaml.load() && tap.load() && iat.load() && consumer.load() &&
               thread_done.load() && !crashed.load();
    }

    // 卸载门（模块侧证据）的派生值。**这是唯一的写法**：没有任何调用方能把结论改成 1。
    std::uint32_t derived_unload_safe() const { return all_proven() ? 1u : 0u; }

    // 失败位图（只用于日志/诊断，不参与结论）。
    std::uint32_t failed_mask() const {
        std::uint32_t mask = 0;
        if (!worker.load()) mask |= 1u << 0;
        if (!xaml.load()) mask |= 1u << 1;
        if (!tap.load()) mask |= 1u << 2;
        if (!iat.load()) mask |= 1u << 3;
        if (!consumer.load()) mask |= 1u << 4;
        if (!thread_done.load()) mask |= 1u << 5;
        if (crashed.load()) mask |= 1u << 6;
        return mask;
    }
};

}  // namespace deskfeed::payload
