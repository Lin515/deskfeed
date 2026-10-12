#include "master/taskbar_supervisor.h"

#include <memory>
#include <string>
#include <vector>

#include "core/utf8.h"
#include "platform/log.h"
#include "platform/win32_util.h"

// 任务栏组件的主进程侧编排。这里只做三件事：按有界的命令行契约异步地请注入器干活、
// 非阻塞地回收它、再按**当前目标状态**决定下一步。
//
// 两条不许越过的线：
//   * **绝不 TerminateProcess**。一次注入可能正在别人的地址空间里改东西；强杀它比让它跑完
//     危险得多。超时只记日志，句柄留到它自己退出（系统会回收）。
//   * **绝不阻塞等待**。回收用零等待看一眼；收尾只请求一次停止，不等注入器跑完。

namespace deskfeed {
namespace {

constexpr wchar_t kDefaultInjectorName[] = L"deskfeed-inject.exe";
constexpr unsigned kMinIntervalMs = 1000;
constexpr unsigned kMaxIntervalMs = 60 * 60 * 1000;

// `--status` 的退出码：0 = 已装载，1 = 未装载（正常），其它 = 问不出来。
constexpr DWORD kStatusLoaded = 0;
constexpr DWORD kStatusNotLoaded = 1;

bool file_exists(std::wstring const& path) {
    if (path.empty()) {
        return false;
    }
    DWORD const attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring directory_of(std::wstring const& path) {
    std::size_t const slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring quote(std::wstring const& text) { return L"\"" + text + L"\""; }

}  // namespace

char const* taskbar_action_name(TaskbarAction action) {
    switch (action) {
        case TaskbarAction::Load: return "load";
        case TaskbarAction::Stop: return "stop";
        case TaskbarAction::Status: return "status";
    }
    return "unknown";
}

unsigned long current_taskbar_owner_pid() {
    // 只认任务栏自己的窗口：`Shell_TrayWnd` 是主任务栏，`Shell_SecondaryTrayWnd` 是副显示器上
    // 那一条。两者都没有时返回 0，调用方据此当作“现在没有可注入的目标”，**不**退化成
    // “进程列表里第一个 explorer.exe”——那条退路会把手伸进一个没有任务栏的 explorer。
    HWND const tray = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray != nullptr) {
        DWORD pid = 0;
        ::GetWindowThreadProcessId(tray, &pid);
        if (pid != 0) {
            return pid;
        }
    }
    HWND const secondary = ::FindWindowW(L"Shell_SecondaryTrayWnd", nullptr);
    if (secondary != nullptr) {
        DWORD pid = 0;
        ::GetWindowThreadProcessId(secondary, &pid);
        if (pid != 0) {
            return pid;
        }
    }
    return 0;
}

TaskbarSupervisor::~TaskbarSupervisor() {
    close_child();
}

bool TaskbarSupervisor::start(TaskbarSupervisorOptions const& options, bool enabled,
                              std::string& error) {
    if (started_) {
        error = "the taskbar supervisor is already started";
        return false;
    }
    if (options.master_pid == 0) {
        error = "master pid is required";
        return false;
    }
    if (options.retry_interval_ms < kMinIntervalMs || options.retry_interval_ms > kMaxIntervalMs) {
        error = "retry interval is out of range";
        return false;
    }
    if (options.check_interval_ms < kMinIntervalMs || options.check_interval_ms > kMaxIntervalMs) {
        error = "check interval is out of range";
        return false;
    }
    if (options.overdue_ms < kMinIntervalMs) {
        error = "overdue budget is out of range";
        return false;
    }

    options_ = options;
    if (options_.injector_path.empty()) {
        options_.injector_path = join_path(directory_of(module_self_path()), kDefaultInjectorName);
    }
    // 先把“注入器在不在”问清楚：这条 seam 一旦成立，第一次动作就会真的去碰别的进程，
    // 与其让它在后台默默失败，不如让调用方在提交配置之前就知道它不成立。
    if (!file_exists(options_.injector_path)) {
        error = "the injector was not found: " + utf8_from_wide(options_.injector_path);
        return false;
    }

    started_ = true;
    // 初始目标状态由调用方给定（见头文件里的理由）。组件本来就关着时：不发动作、不问状态，
    // 只是把能力（注入器在不在）问清楚并武装起来，等用户打开时再按同一套状态机走。
    enabled_ = enabled;
    shutting_down_ = false;
    stop_requested_ = false;
    pending_load_ = false;
    pending_stop_ = false;
    retry_pending_ = false;
    retry_at_ms_ = 0;
    need_check_ = enabled;  // 组件开着：第一次 tick 先问一次状态，别盲装
    next_check_ms_ = 0;
    last_owner_pid_ = 0;
    log_format(L"taskbar: supervisor armed (enabled=%s injector=%s scope=%s master pid=%lu retry=%u ms)",
               enabled ? L"yes" : L"no", options_.injector_path.c_str(),
               options_.scope.empty() ? L"(production)" : options_.scope.c_str(),
               options_.master_pid, options_.retry_interval_ms);
    return true;
}

bool TaskbarSupervisor::set_enabled(bool enabled) {
    if (!started_) {
        // 没武装过（--no-inject / --headless / 配置不成立）：开关只能报“关”，
        // 而且一个字节都不许碰目标进程。
        enabled_ = false;
        return false;
    }
    if (enabled) {
        if (!enabled_) {
            log_line(L"taskbar: component switch is on; the payload will be loaded again");
        }
        enabled_ = true;
        // 用户又要它了：撤掉还没发出去的停止令牌（那是一次已经过期的意图），并问一次状态——
        // 停止可能正好在跑，回来时负载已经不在了，问一次才知道要不要装回去。
        pending_stop_ = false;
        need_check_ = true;
        next_check_ms_ = 0;
        retry_at_ms_ = 0;
        return true;
    }
    if (enabled_) {
        log_line(L"taskbar: component switch is off; unloading the payload and stopping retries");
    }
    enabled_ = false;
    pending_load_ = false;
    retry_pending_ = false;
    // 落停止令牌。它**不会**因为“此刻正好有一个动作在跑”而丢：那个动作结束之后，
    // tick 会优先把它发出去（`--load` 正在跑时也要等它跑完再停，顺序不能反）。
    pending_stop_ = true;
    return false;
}

void TaskbarSupervisor::tick(long long now_ms) {
    if (!started_) {
        return;
    }

    // 先回收：已经退出的注入器不占名额，也不留句柄。
    if (child_ != nullptr) {
        if (::WaitForSingleObject(child_->process, 0) == WAIT_OBJECT_0) {
            DWORD exit_code = 0;
            ::GetExitCodeProcess(child_->process, &exit_code);
            TaskbarAction const action = child_->action;
            bool const overdue = child_->overdue_logged;
            log_format(L"taskbar: --%s finished (exit code %lu%s)",
                       wide_from_utf8(taskbar_action_name(action)).c_str(), exit_code,
                       overdue ? L", past its budget" : L"");
            close_child();
            on_child_exit(action, exit_code, now_ms);
        } else if (!child_->overdue_logged &&
                   now_ms - child_->started_ms >= static_cast<long long>(options_.overdue_ms)) {
            // 只是记一行：不杀、不等、不重试。它自己跑完会走上面的回收分支。
            child_->overdue_logged = true;
            log_format(L"taskbar: --%s is taking longer than %u ms; still waiting for it to finish",
                       wide_from_utf8(taskbar_action_name(child_->action)).c_str(),
                       options_.overdue_ms);
        }
    }

    if (child_ != nullptr) {
        return;  // 同时只允许一个在跑；下面要发的动作等它结束
    }

    // 动作优先级：**先停**（用户的关闭意图、以及收尾，都不能被一次装载盖过去），
    // 再按需问状态/装载。收尾闩只挡住“开新动作”，不挡“把还没发出去的停止发掉”。
    if (pending_stop_) {
        pending_stop_ = false;
        stop_requested_ = true;
        request(TaskbarAction::Stop, now_ms);
        return;
    }
    if (shutting_down_) {
        return;  // 收尾闩：不再开新的状态问询或装载
    }
    if (!enabled_) {
        return;
    }
    if (retry_pending_ && now_ms < retry_at_ms_) {
        return;  // 还没到下一次重试
    }
    bool const due_for_check = need_check_ || pending_load_ || now_ms >= next_check_ms_;
    if (due_for_check) {
        next_check_ms_ = now_ms + static_cast<long long>(options_.check_interval_ms);
        request(TaskbarAction::Status, now_ms);
        return;
    }
}

void TaskbarSupervisor::on_child_exit(TaskbarAction action, DWORD exit_code, long long now_ms) {
    // 每一次完成处理都按**此刻的目标状态**重新决策，绝不按发起那一刻的旧意图。
    switch (action) {
        case TaskbarAction::Status: {
            need_check_ = false;
            unsigned long const owner = current_taskbar_owner_pid();
            if (owner != last_owner_pid_) {
                last_owner_pid_ = owner;
                if (owner != 0) {
                    log_format(L"taskbar: the taskbar owner is now pid %lu", owner);
                } else {
                    log_line(L"taskbar: no taskbar owner right now (Shell_TrayWnd is gone)");
                }
            }
            if (!enabled_ || shutting_down_ || pending_stop_) {
                return;  // 组件已经关了（或正在收尾）：状态说什么都不去装
            }
            if (exit_code == kStatusLoaded) {
                pending_load_ = false;
                retry_pending_ = false;
                return;
            }
            if (exit_code == kStatusNotLoaded) {
                pending_load_ = true;
                request(TaskbarAction::Load, now_ms);
                return;
            }
            // 问不出来（打不开属主、注入器出错）：按退避再问，不盲目装载。
            retry_pending_ = true;
            retry_at_ms_ = now_ms + static_cast<long long>(options_.retry_interval_ms);
            return;
        }
        case TaskbarAction::Load: {
            if (!enabled_ || shutting_down_ || pending_stop_) {
                // 装载是在“用户还想要它”的时候发起的，但完成时用户已经关掉了：**绝不能**
                // 因为这次成功回来就把组件重新打开。停止令牌还在，tick 会接着把它发出去。
                log_line(L"taskbar: --load finished after the component was switched off; "
                         L"the pending stop will run next");
                return;
            }
            if (exit_code == 0) {
                pending_load_ = false;
                retry_pending_ = false;
                log_line(L"taskbar: the payload is in place");
                return;
            }
            log_format(L"taskbar: --load failed (exit code %lu); retrying in %u ms", exit_code,
                       options_.retry_interval_ms);
            retry_pending_ = true;
            retry_at_ms_ = now_ms + static_cast<long long>(options_.retry_interval_ms);
            return;
        }
        case TaskbarAction::Stop: {
            if (exit_code == 0) {
                log_line(L"taskbar: the payload is down");
            } else {
                // 非 0 = 注入器判定“没安全撤下”（例如已经被别的实例接管，或 Stop 报告留着
                // 更安全）。**不重试、不强杀**：再动一次手只会更危险。
                log_format(L"taskbar: --stop did not remove the payload (exit code %lu); leaving it",
                           exit_code);
            }
            if (enabled_ && !shutting_down_) {
                // 停止期间用户又把组件打开了：负载刚被撤下（或没能撤下），都得重新问一次
                // 才知道现在是什么状态，再决定要不要装回去。绝不能让这次停止的完成把
                // “用户已经打开”这件事冻住。
                need_check_ = true;
                next_check_ms_ = 0;
                retry_at_ms_ = 0;
                retry_pending_ = false;
            }
            return;
        }
    }
}

void TaskbarSupervisor::shutdown() {
    if (!started_) {
        return;
    }
    shutting_down_ = true;
    enabled_ = false;
    pending_load_ = false;
    retry_pending_ = false;
    // 收尾要保证“这次运行开的组件会被收掉”，所以这里落的是一次新的停止令牌：
    // 之前那次 --stop 是给“用户关掉组件”用的，不能当成收尾也停过了。
    // 有动作在跑时**不等**——主进程要退出了，等它等于把退出挂在一个我们无法保证的时长上；
    // 令牌会留在那里，那个动作一结束就发（tick 仍会处理 pending_stop_）。
    pending_stop_ = true;
    if (child_ == nullptr) {
        pending_stop_ = false;
        stop_requested_ = true;
        request(TaskbarAction::Stop, 0);
    }
}

void TaskbarSupervisor::request(TaskbarAction action, long long now_ms) {
    if (child_ != nullptr) {
        // 一次动作还在跑：把这次的意图记成令牌，等 tick 在回收之后按优先级发出去。
        if (action == TaskbarAction::Load) {
            pending_load_ = true;
        } else if (action == TaskbarAction::Stop) {
            pending_stop_ = true;
        }
        return;
    }
    std::string error;
    if (!spawn(action, error)) {
        log_line(L"taskbar: " + wide_from_utf8(error));
        if (action == TaskbarAction::Load) {
            retry_pending_ = true;
            retry_at_ms_ = now_ms + static_cast<long long>(options_.retry_interval_ms);
        }
        return;
    }
    if (action == TaskbarAction::Load) {
        pending_load_ = false;
    }
}

bool TaskbarSupervisor::spawn(TaskbarAction action, std::string& error) {
    std::wstring command = quote(options_.injector_path);
    command += L" --";
    command += wide_from_utf8(taskbar_action_name(action));
    if (!options_.scope.empty()) {
        command += L" --scope " + options_.scope;
    }
    if (action != TaskbarAction::Status && !options_.payload_path.empty()) {
        command += L" --payload " + quote(options_.payload_path);
    }

    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    // 先把这次动作的记录准备好，再启动进程：`new` 万一抛（bad_alloc），此刻还没有子进程，
    // 也就不存在“进程起来了、句柄没人收”的泄漏窗口。
    auto child = std::make_unique<Child>();
    child->action = action;
    child->started_ms = static_cast<long long>(::GetTickCount64());

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    // CREATE_NO_WINDOW：注入器是个控制台程序，但它不该在用户桌面上闪一个窗口。
    if (::CreateProcessW(options_.injector_path.c_str(), mutable_command.data(), nullptr, nullptr,
                         FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info) == FALSE) {
        error = "cannot start the injector: " + utf8_from_wide(last_error_text());
        return false;
    }
    ::CloseHandle(info.hThread);
    child->process = info.hProcess;
    child->pid = info.dwProcessId;
    child_ = child.release();
    log_format(L"taskbar: --%s requested (pid %lu)",
               wide_from_utf8(taskbar_action_name(action)).c_str(), child_->pid);
    return true;
}

void TaskbarSupervisor::close_child() {
    if (child_ == nullptr) {
        return;
    }
    if (child_->process != nullptr) {
        ::CloseHandle(child_->process);
        child_->process = nullptr;
    }
    delete child_;
    child_ = nullptr;
}

}  // namespace deskfeed
