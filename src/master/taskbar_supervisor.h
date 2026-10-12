#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

// 任务栏组件的**主进程侧编排**：什么时候把载荷装进任务栏、什么时候撤下来。
//
// 责任边界（见 docs/taskbar-injection.md 的“任务栏组件开关”）：
//   * 这里**不做**注入。真正的“找任务栏属主、装载/卸载负载、清理别的 explorer 里的残留”
//     全部在注入器（`deskfeed-inject.exe`）里，那是它对别的进程动手的独占能力；
//     主进程只按一份有界的命令行契约异步地请它干活，不链接它的代码。
//   * 这里**不碰界面**，也不碰显示邮箱：任务栏那块的读数由负载 DLL 自己从显示邮箱读最新帧。
//   * 这里**不轮询目标进程的模块表**（那是注入器 `--status` 的事），只在有界退避之后问一次。
//
// 异步与收尾纪律：
//   * 每次动作都是一次有界的 CreateProcessW；同时只允许一个在跑。
//   * 子进程句柄一直持有到它自己退出，主循环每轮用零等待看一眼（`tick()`）——**绝不阻塞等待、
//     绝不 TerminateProcess**。超时只记一行日志并放弃这次等待，进程留给系统收；因为一次注入
//     可能正在别人的地址空间里改东西，强杀它比让它跑完危险得多。
//   * 卸载的判据是注入器自己的结论：**退出码 0 才算“已安全撤下”**；非 0（包括负载已被别的
//     实例接管、或 Stop 报告“留着更安全”）一律视为未撤下——编排层不重试、不强杀，只记日志。
//   * 收尾只**请求**一次停止（一次异步 `--stop`），不等待、不重试。
//
// 状态模型（这里是这份编排最容易写错的地方，所以把三件事分开）：
//   * `enabled_`：**用户/组件当前想要的目标状态**，只有 set_enabled 改它。
//   * `pending_stop_` / `pending_load_`：还没发出去的动作令牌。用户关掉组件时先落
//     `pending_stop_`；它**不会**因为“当时正好有一个动作在跑”而丢——那个动作一结束就优先发它。
//   * `shutting_down_`：收尾闩。置上之后不再开新动作，也不再被 set_enabled 清掉。
//   任何一次动作的**完成处理**都按“此刻的目标状态”重新决策，而不是按发起那一刻的旧意图：
//   关闭组件时正在跑的那次 `--load` 成功回来，也**不会**把它重新打开。
namespace deskfeed {

// 注入器的动作。命令行契约：
//   deskfeed-inject.exe --load  [--payload <DLL>] [--scope <NAME>]
//   deskfeed-inject.exe --stop  [--payload <DLL>] [--scope <NAME>]
//   deskfeed-inject.exe --status [--scope <NAME>]
// 目标一律是**当前任务栏属主**（`Shell_TrayWnd` / `Shell_SecondaryTrayWnd` 的属主进程），
// 没有“进程列表里第一个 explorer.exe”这条退路。scope 为空 = 生产实例（命令行里不出现
// `--scope`），非空时必须是 1..32 个 [A-Za-z0-9_-] 字符。
//
// 退出码：`--status` 用 0 = 已装载 / 1 = 未装载（正常）/ 其它 = 问不出来；`--load` 与
// `--stop` 用 0 = 成功，非 0 = 没做到（含“没安全撤下，留着更安全”）。
enum class TaskbarAction {
    Load = 0,
    Stop = 1,
    Status = 2,
};

char const* taskbar_action_name(TaskbarAction action);

// `--status` 的三种结论（由注入器的退出码给出）。
enum class TaskbarStatus {
    Loaded = 0,     // 退出码 0：负载已经在它该在的地方
    NotLoaded = 1,  // 退出码 1：没装上（正常状态，不是错误）
    Failed = 2,     // 其它退出码：问不出来（打不开属主、注入器自己出错）
};

// 当前任务栏属主的 PID：`Shell_TrayWnd`，没有就退到 `Shell_SecondaryTrayWnd`；
// 都没有返回 0。**不做**“第一个 explorer.exe”这种猜测。
unsigned long current_taskbar_owner_pid();

struct TaskbarSupervisorOptions {
    // 实例 scope：与主进程、控制窗口、显示邮箱同一份命名（空 = 生产实例）。
    std::wstring scope;
    // 主进程自己的 PID：负载据此绑定生产者、等它退出后自清理。
    unsigned long master_pid = 0;
    // 负载 DLL 的路径。空 = 由注入器按自己的默认规则找（与注入器同目录）。
    std::wstring payload_path;
    // 注入器可执行文件。空 = 与主进程同目录下的 deskfeed-inject.exe。
    std::wstring injector_path;
    // 一次动作失败之后多久再试（毫秒），下限 1000。
    unsigned retry_interval_ms = 5000;
    // 两次 `--status` 之间至少隔多久（毫秒），下限 1000。
    unsigned check_interval_ms = 30000;
    // 一次注入器调用超过这个时长只记一行日志（不杀、不重试）。
    unsigned overdue_ms = 15000;
};

class TaskbarSupervisor {
public:
    TaskbarSupervisor() = default;
    ~TaskbarSupervisor();
    TaskbarSupervisor(TaskbarSupervisor const&) = delete;
    TaskbarSupervisor& operator=(TaskbarSupervisor const&) = delete;

    // 记住配置，并把组件的**初始目标状态**设为 `enabled`。返回 false 时配置不成立（PID 为 0、
    // 间隔越界、注入器不存在）——调用方据此把任务栏组件判为“本次运行不可用”，绝不在配置
    // 不成立的情况下留下注入副作用。
    //
    // 为什么初始状态是参数而不是“先 start 再 set_enabled(false)”：后者会落一个待发的停止
    // 令牌，于是“用户上次就把组件关了”的一次普通启动也会去 spawn 一次 `--stop`。组件本来就
    // 关着的时候，启动不该对别的进程做任何动作；而 `enabled = false` 的武装会一次动作都不发。
    bool start(TaskbarSupervisorOptions const& options, bool enabled, std::string& error);

    // 组件开关：改的是**目标状态**，不阻塞，也绝不在这里等待注入器。
    //   开：清掉待发的停止令牌（用户又要它了），按需问一次状态。
    //   关：清掉待发的装载令牌并落下停止令牌；已经有动作在跑时，那个动作一结束就发停止。
    // 返回拨动之后的目标状态。
    bool set_enabled(bool enabled);

    bool enabled() const { return enabled_; }
    bool started() const { return started_; }
    bool action_in_flight() const { return child_ != nullptr; }

    // 主循环每轮调一次：非阻塞地回收已经退出的注入器，再按**当前目标状态**决定下一步动作。
    // `now_ms` 是单调时钟（GetTickCount64 那一域）。
    void tick(long long now_ms);

    // 收尾：置收尾闩、只请求一次停止（异步），不等待、不重试。析构只关句柄。
    void shutdown();

private:
    struct Child {
        HANDLE process = nullptr;
        unsigned long pid = 0;
        TaskbarAction action = TaskbarAction::Load;
        long long started_ms = 0;
        bool overdue_logged = false;
    };

    void request(TaskbarAction action, long long now_ms);
    bool spawn(TaskbarAction action, std::string& error);
    void close_child();
    void on_child_exit(TaskbarAction action, DWORD exit_code, long long now_ms);

    TaskbarSupervisorOptions options_{};
    bool started_ = false;
    bool enabled_ = false;        // 目标状态
    bool shutting_down_ = false;  // 收尾闩：不再开新动作
    bool stop_requested_ = false; // 已经真的发过一次 --stop（收尾只发一次）
    bool pending_load_ = false;   // 待发：装载
    bool pending_stop_ = false;   // 待发：停止（优先于装载）
    bool retry_pending_ = false;  // 上一次动作没成功：按退避再试
    long long retry_at_ms_ = 0;
    long long next_check_ms_ = 0;  // 下一次允许问 --status 的时刻
    bool need_check_ = false;      // 目标状态变了：下一轮 tick 先问一次状态
    unsigned long last_owner_pid_ = 0;
    Child* child_ = nullptr;  // 同一时刻最多一个
};

}  // namespace deskfeed
