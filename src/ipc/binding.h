#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

// 生产者进程绑定：持 SYNCHRONIZE 句柄等待退出，不轮询 PID。
//
// 绑定带代次。旧生产者的退出回调一定带着它自己那一代的号；主进程只认当前代次，
// 所以“旧绑定退出”不会关掉已经切到新生产者的实例（见 docs/lifecycle.md）。
//
// 换绑是两阶段的：**先**把新的等待者准备好（准备阶段失败不碰旧绑定），**再**提交。
// 配置落盘失败时可以直接 discard，旧绑定与旧运行时状态一个字节都不变。
namespace deskfeed {

class ProcessBinding {
public:
    struct Prepared {
        HANDLE process = nullptr;
        HANDLE cancel = nullptr;
        HANDLE thread = nullptr;
        unsigned long pid = 0;
        std::uint64_t generation = 0;
        bool armed = false;  // true = 归本结构所有，discard 会关掉
    };

    ProcessBinding() = default;
    ~ProcessBinding();
    ProcessBinding(ProcessBinding const&) = delete;
    ProcessBinding& operator=(ProcessBinding const&) = delete;

    // 准备等待者。process 必须是已经打开的 SYNCHRONIZE 句柄：
    // 成功时所有权转入 prepared，失败时仍归调用方（调用方负责关闭）。
    static bool prepare(HANDLE process, unsigned long pid, std::uint64_t generation, HWND notify,
                        UINT message, Prepared& prepared, std::string& error);

    // 放弃一个已准备但未提交的等待者（含它持有的进程句柄）。
    static void discard(Prepared& prepared);

    // 用准备好的等待者替换当前绑定。这一步不分配资源，不会失败。
    void commit(Prepared& prepared);

    void clear();

    bool active() const { return pid_ != 0; }
    unsigned long pid() const { return pid_; }
    std::uint64_t generation() const { return generation_; }

    // 当前绑定的进程**真的已经退出**了吗（对着实际的 SYNCHRONIZE 句柄问一次）。
    //
    // 只按代次判定是不够的：prepare 会立刻启动等待者，所以一次最终**没有提交**的绑定尝试
    // （例如落盘失败被 discard）也可能已经把自己的代次投递到消息队列里，而 discard 收不回
    // 已经排队的窗口消息。因此退出回调除了核对代次，还必须确认“当前绑定的这个进程句柄
    // 确实已经触发”。两条件同时成立才收尾。
    bool signaled() const;

private:
    HANDLE process_ = nullptr;
    HANDLE cancel_ = nullptr;
    HANDLE thread_ = nullptr;
    unsigned long pid_ = 0;
    std::uint64_t generation_ = 0;
};

}  // namespace deskfeed
