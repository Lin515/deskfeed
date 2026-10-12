// 任务栏编排（src/master/taskbar_supervisor.cpp）的**永久离线用例**。
//
// 它驱动**真实**的 TaskbarSupervisor，被调对象是 `deskfeed-fake-injector.exe`——一个只写日志、
// 按环境变量决定退出码与延迟的 console 桩。那个桩不打开进程、不写内存、不注入、不联网、不起
// 任何窗口，所以这整套用例可以在没有 explorer、没有界面、没有真机授权的情况下跑。
//
// 断言的对象是日志的**行序**：什么时候发了哪个动作、一次动作还在跑时下一步等不等它。
// 顺序就是编排层的决策顺序，因此它同时能分辨“状态模型写对了”和“只是碰巧没崩”。
//
// 独立性：这是一个**独立可执行文件**（自己带极简框架的 main），刻意与 tests/tests.cpp 分开。
// 它需要 `src/master/taskbar_supervisor.cpp` 一起编进这个目标（见 CMake 里的说明）。
//
// 运行：deskfeed-supervisor-tests.exe [--filter <子串>]
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "master/taskbar_supervisor.h"
#include "test_support.h"

namespace testing {

// 极简框架的存储与失败上报：与 tests/tests.cpp 同一份契约（见 tests/test_support.h）。
// 两个目标各自提供一份，因为它们**不**链接同一个可执行文件。
std::vector<Case>& cases() {
    static std::vector<Case> list;
    return list;
}

[[noreturn]] void report_failure(std::string message) { throw Failure{std::move(message)}; }

}  // namespace testing

namespace {

using deskfeed::TaskbarSupervisor;
using deskfeed::TaskbarSupervisorOptions;

long long monotonic_ms() { return static_cast<long long>(::GetTickCount64()); }

std::wstring module_directory() {
    wchar_t buffer[MAX_PATH]{};
    DWORD const chars = ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    std::wstring const self(buffer, chars);
    std::size_t const slash = self.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : self.substr(0, slash + 1);
}

// 假注入器与用例在同一个 bin 目录里（CMake 把两个目标输出到同一处）。
std::wstring fake_injector_path() { return module_directory() + L"deskfeed-fake-injector.exe"; }

void set_environment(std::wstring const& name, std::wstring const& value) {
    ::SetEnvironmentVariableW(name.c_str(), value.c_str());
}

void set_environment(wchar_t const* name, wchar_t const* value) {
    ::SetEnvironmentVariableW(name, value);
}

// 隔离的临时目录：每次都换一个名字，收尾只删自己写进去的东西。
class TempDirectory {
public:
    TempDirectory() {
        wchar_t base[MAX_PATH]{};
        ::GetTempPathW(static_cast<DWORD>(std::size(base)), base);
        path_ = std::wstring(base) + L"deskfeed-supervisor-" +
                std::to_wstring(::GetCurrentProcessId()) + L"-" +
                std::to_wstring(++counter_);
        ::CreateDirectoryW(path_.c_str(), nullptr);
    }
    ~TempDirectory() { remove(); }

    TempDirectory(TempDirectory const&) = delete;
    TempDirectory& operator=(TempDirectory const&) = delete;

    std::wstring join(std::wstring const& name) const { return path_ + L"\\" + name; }
    std::wstring const& path() const { return path_; }

private:
    void remove() const {
        std::wstring const pattern = path_ + L"\\*";
        WIN32_FIND_DATAW found{};
        HANDLE const search = ::FindFirstFileW(pattern.c_str(), &found);
        if (search != INVALID_HANDLE_VALUE) {
            do {
                if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                    ::DeleteFileW((path_ + L"\\" + found.cFileName).c_str());
                }
            } while (::FindNextFileW(search, &found));
            ::FindClose(search);
        }
        ::RemoveDirectoryW(path_.c_str());
    }

    std::wstring path_;
    static int counter_;
};

int TempDirectory::counter_ = 0;

// 读日志：一行一个动作（假注入器原样记下整条命令行）。文件不存在 = 还没有动作。
std::vector<std::string> read_log(std::wstring const& path) {
    std::vector<std::string> lines;
    HANDLE const file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return lines;
    }
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 20)) {
        std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
        DWORD read = 0;
        if (::ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) == TRUE) {
            bytes.resize(read);
            std::string line;
            for (char const ch : bytes) {
                if (ch == '\n') {
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    lines.push_back(line);
                    line.clear();
                } else {
                    line.push_back(ch);
                }
            }
            if (!line.empty()) {
                lines.push_back(line);
            }
        }
    }
    ::CloseHandle(file);
    return lines;
}

// 每个场景共用的编排层配置：scope 非空（隔离实例）、注入器指向假桩、退避都是下限。
TaskbarSupervisorOptions options_for(std::wstring const& scope) {
    TaskbarSupervisorOptions options;
    options.scope = scope;
    options.master_pid = ::GetCurrentProcessId();
    options.injector_path = fake_injector_path();
    options.retry_interval_ms = 1000;
    options.check_interval_ms = 1000;  // 让“下一次状态问询”在用例里也能等到
    options.overdue_ms = 1000;
    return options;
}

// 驱动编排层直到日志的前 expected.size() 行与期望完全一致（顺序与内容都要对）。
// tick 是非阻塞的，所以这里必须反复推它；不满足就在 timeout 之后失败。
bool pump_until_log(TaskbarSupervisor& supervisor, std::wstring const& log_path,
                    std::vector<std::string> const& expected, int timeout_ms) {
    long long const deadline = monotonic_ms() + timeout_ms;
    for (;;) {
        supervisor.tick(monotonic_ms());
        std::vector<std::string> const lines = read_log(log_path);
        if (lines.size() >= expected.size()) {
            bool matched = true;
            for (std::size_t index = 0; index < expected.size(); ++index) {
                if (lines[index] != expected[index]) {
                    matched = false;
                    break;
                }
            }
            if (matched) {
                return true;
            }
        }
        if (monotonic_ms() >= deadline) {
            return false;
        }
        ::Sleep(5);
    }
}

// 驱动编排层直到日志至少 min_lines 行、并且此刻没有动作在跑。用来把“某一次动作已经彻底
// 回收”变成一个确定的前提，免得用例在时序上猜。
bool pump_until_idle(TaskbarSupervisor& supervisor, std::wstring const& log_path,
                     std::size_t min_lines, int timeout_ms) {
    long long const deadline = monotonic_ms() + timeout_ms;
    while (monotonic_ms() < deadline) {
        supervisor.tick(monotonic_ms());
        if (!supervisor.action_in_flight() && read_log(log_path).size() >= min_lines) {
            return true;
        }
        ::Sleep(5);
    }
    return false;
}

// 收尾：把还没回收的子进程推完（不等它跑完业务，只是别把进程留在别人机器上）。
void settle(TaskbarSupervisor& supervisor, int timeout_ms) {
    long long const deadline = monotonic_ms() + timeout_ms;
    while (monotonic_ms() < deadline) {
        supervisor.tick(monotonic_ms());
        if (!supervisor.action_in_flight()) {
            supervisor.tick(monotonic_ms());
            if (!supervisor.action_in_flight()) {
                return;
            }
        }
        ::Sleep(5);
    }
}

// 一次动作的命令行长这样（假注入器原样记录）：`--status --scope NAME`。
std::string line_for(char const* action, std::wstring const& scope) {
    return std::string(action) + " --scope " + testing::to_display(scope);
}

// 汇总一份可读的日志用于失败信息（顺序错了要能一眼看出错在哪）。
std::string join_log(std::vector<std::string> const& lines) {
    std::string out;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index > 0) {
            out += " | ";
        }
        out += lines[index];
    }
    return out.empty() ? std::string("<empty>") : out;
}

}  // namespace

// ---------------------------------------------------------------------------
// 健康路径
// ---------------------------------------------------------------------------

TEST(supervisor_loads_the_payload_when_the_component_starts_enabled) {
    TempDirectory dir;
    std::wstring const scope = L"sup-load";
    std::wstring const log = dir.join(L"actions.log");
    set_environment(L"DESKFEED_FAKE_LOG", log);
    set_environment(L"DESKFEED_FAKE_STATUS", L"1");  // 未装载 → 该装
    set_environment(L"DESKFEED_FAKE_LOAD", L"0");
    set_environment(L"DESKFEED_FAKE_STOP", L"0");
    set_environment(L"DESKFEED_FAKE_DELAY_MS", L"0");

    TaskbarSupervisor supervisor;
    std::string error;
    CHECK(supervisor.start(options_for(scope), true, error));
    CHECK(supervisor.started());
    CHECK(supervisor.enabled());

    std::vector<std::string> const expected{line_for("--status", scope), line_for("--load", scope)};
    bool const matched = pump_until_log(supervisor, log, expected, 10000);
    if (!matched) {
        testing::report_failure("期望的顺序没有出现：" + join_log(read_log(log)));
    }
    settle(supervisor, 5000);
}

TEST(supervisor_does_not_load_when_the_payload_is_already_in_place) {
    TempDirectory dir;
    std::wstring const scope = L"sup-already";
    std::wstring const log = dir.join(L"actions.log");
    set_environment(L"DESKFEED_FAKE_LOG", log);
    set_environment(L"DESKFEED_FAKE_STATUS", L"0");  // 已装载

    TaskbarSupervisor supervisor;
    std::string error;
    CHECK(supervisor.start(options_for(scope), true, error));

    std::vector<std::string> const expected{line_for("--status", scope)};
    if (!pump_until_log(supervisor, log, expected, 10000)) {
        testing::report_failure("连状态问询都没有发生：" + join_log(read_log(log)));
    }
    // 再推一段：已经装载就绝不该出现 --load。
    long long const deadline = monotonic_ms() + 600;
    while (monotonic_ms() < deadline) {
        supervisor.tick(monotonic_ms());
        ::Sleep(5);
    }
    std::vector<std::string> const lines = read_log(log);
    CHECK_EQ(lines.size(), static_cast<std::size_t>(1));
    CHECK(lines[0].find("--load") == std::string::npos);
    settle(supervisor, 5000);
}

// ---------------------------------------------------------------------------
// 开关：关掉、再打开
// ---------------------------------------------------------------------------

TEST(supervisor_stops_the_payload_when_the_component_is_switched_off) {
    TempDirectory dir;
    std::wstring const scope = L"sup-off";
    std::wstring const log = dir.join(L"actions.log");
    set_environment(L"DESKFEED_FAKE_LOG", log);
    set_environment(L"DESKFEED_FAKE_STATUS", L"0");  // 已装载 → 关掉时要撤下来

    TaskbarSupervisor supervisor;
    std::string error;
    CHECK(supervisor.start(options_for(scope), true, error));
    if (!pump_until_log(supervisor, log, {line_for("--status", scope)}, 10000)) {
        testing::report_failure("第一次状态问询没有发生：" + join_log(read_log(log)));
    }

    CHECK(!supervisor.set_enabled(false));  // 返回拨动之后的目标状态
    CHECK(!supervisor.enabled());

    std::vector<std::string> const expected{line_for("--status", scope), line_for("--stop", scope)};
    if (!pump_until_log(supervisor, log, expected, 10000)) {
        testing::report_failure("关掉组件之后没有发出停止：" + join_log(read_log(log)));
    }
    settle(supervisor, 5000);
}

TEST(supervisor_reopens_and_reloads_after_being_switched_off) {
    TempDirectory dir;
    std::wstring const scope = L"sup-reopen";
    std::wstring const log = dir.join(L"actions.log");
    set_environment(L"DESKFEED_FAKE_LOG", log);
    set_environment(L"DESKFEED_FAKE_STATUS", L"1");  // 未装载 → 每次打开都该装

    TaskbarSupervisor supervisor;
    std::string error;
    CHECK(supervisor.start(options_for(scope), true, error));

    std::vector<std::string> const loaded{line_for("--status", scope), line_for("--load", scope)};
    if (!pump_until_log(supervisor, log, loaded, 10000)) {
        testing::report_failure("第一次装载没有完成：" + join_log(read_log(log)));
    }

    supervisor.set_enabled(false);
    std::vector<std::string> const stopped{line_for("--status", scope), line_for("--load", scope),
                                           line_for("--stop", scope)};
    if (!pump_until_log(supervisor, log, stopped, 10000)) {
        testing::report_failure("关掉组件之后没有发出停止：" + join_log(read_log(log)));
    }

    // 再打开：停止是**一次性意图**，不是永久闩。必须重新问状态并装回去。
    CHECK(supervisor.set_enabled(true));
    std::vector<std::string> const reopened{line_for("--status", scope), line_for("--load", scope),
                                            line_for("--stop", scope),
                                            line_for("--status", scope), line_for("--load", scope)};
    if (!pump_until_log(supervisor, log, reopened, 10000)) {
        testing::report_failure("重新打开之后没有装回去：" + join_log(read_log(log)));
    }
    settle(supervisor, 5000);
}

// ---------------------------------------------------------------------------
// 待发令牌：动作还在跑时落下的意图不许丢
// ---------------------------------------------------------------------------

TEST(supervisor_does_not_lose_a_stop_that_arrives_while_a_load_is_running) {
    TempDirectory dir;
    std::wstring const scope = L"sup-pending-stop";
    std::wstring const log = dir.join(L"actions.log");
    set_environment(L"DESKFEED_FAKE_LOG", log);
    set_environment(L"DESKFEED_FAKE_STATUS", L"1");  // 未装载
    // 让第一次状态问询慢下来，好让“关掉组件”落在它还在跑的时候。
    set_environment(L"DESKFEED_FAKE_DELAY_MS", L"600");

    TaskbarSupervisor supervisor;
    std::string error;
    CHECK(supervisor.start(options_for(scope), true, error));
    supervisor.tick(monotonic_ms());  // 发出 --status，此刻它在跑
    CHECK(supervisor.action_in_flight());

    // 用户在这个窗口里关掉了组件：停止令牌必须落下来，而且**优先于**这次状态问询可能引发的装载。
    supervisor.set_enabled(false);

    std::vector<std::string> const expected{line_for("--status", scope), line_for("--stop", scope)};
    if (!pump_until_log(supervisor, log, expected, 10000)) {
        testing::report_failure("停止令牌丢了：" + join_log(read_log(log)));
    }
    std::vector<std::string> const lines = read_log(log);
    for (std::string const& line : lines) {
        // 关掉组件之后绝不能再装：状态问询回来时组件已经关了。
        CHECK(line.find("--load") == std::string::npos);
    }
    settle(supervisor, 5000);
}

TEST(supervisor_rechecks_after_a_stop_when_the_component_was_turned_back_on) {
    TempDirectory dir;
    std::wstring const scope = L"sup-recheck";
    std::wstring const log = dir.join(L"actions.log");
    set_environment(L"DESKFEED_FAKE_LOG", log);
    set_environment(L"DESKFEED_FAKE_STATUS", L"0");  // 已装载 → 停止完成后重新问，会发现仍装载
    // 让停止慢下来，好让“用户又把组件打开”真的落在它还在跑的时候。
    set_environment(L"DESKFEED_FAKE_DELAY_MS", L"600");

    TaskbarSupervisor supervisor;
    std::string error;
    CHECK(supervisor.start(options_for(scope), true, error));
    if (!pump_until_idle(supervisor, log, 1, 10000)) {
        testing::report_failure("第一次状态问询没有收干净：" + join_log(read_log(log)));
    }

    // 关掉：停止被发出去，此刻它在跑。
    supervisor.set_enabled(false);
    supervisor.tick(monotonic_ms());
    CHECK(supervisor.action_in_flight());
    // 停止还在跑的时候用户又把组件打开了：这次停止的完成**不能**把新状态冻住。
    supervisor.set_enabled(true);

    std::vector<std::string> const expected{line_for("--status", scope), line_for("--stop", scope),
                                            line_for("--status", scope)};
    if (!pump_until_log(supervisor, log, expected, 15000)) {
        testing::report_failure("停止之后没有重新问状态：" + join_log(read_log(log)));
    }
    settle(supervisor, 5000);
}

// ---------------------------------------------------------------------------
// 收尾
// ---------------------------------------------------------------------------

TEST(supervisor_sends_a_fresh_stop_on_shutdown_after_the_user_already_switched_it_off) {
    TempDirectory dir;
    std::wstring const scope = L"sup-shutdown";
    std::wstring const log = dir.join(L"actions.log");
    set_environment(L"DESKFEED_FAKE_LOG", log);
    set_environment(L"DESKFEED_FAKE_STATUS", L"0");

    TaskbarSupervisor supervisor;
    std::string error;
    CHECK(supervisor.start(options_for(scope), true, error));
    if (!pump_until_log(supervisor, log, {line_for("--status", scope)}, 10000)) {
        testing::report_failure("状态问询没有发生：" + join_log(read_log(log)));
    }
    supervisor.set_enabled(false);
    std::vector<std::string> const stopped{line_for("--status", scope), line_for("--stop", scope)};
    if (!pump_until_log(supervisor, log, stopped, 10000)) {
        testing::report_failure("用户关掉时的停止没有发出：" + join_log(read_log(log)));
    }

    // 关键点：用户那次关闭用的是一次性的停止，**不算收尾也停过了**。收尾必须再发一次——
    // 这次运行期间组件被打开过，那就有东西要撤。旧实现在这里发不出第二次停止。
    supervisor.shutdown();
    std::vector<std::string> const drained{line_for("--status", scope), line_for("--stop", scope),
                                           line_for("--stop", scope)};
    if (!pump_until_log(supervisor, log, drained, 10000)) {
        testing::report_failure("收尾没有发出新的停止：" + join_log(read_log(log)));
    }
    settle(supervisor, 5000);
}

TEST(supervisor_shutdown_does_not_wait_for_a_running_action) {
    TempDirectory dir;
    std::wstring const scope = L"sup-nowait";
    std::wstring const log = dir.join(L"actions.log");
    set_environment(L"DESKFEED_FAKE_LOG", log);
    set_environment(L"DESKFEED_FAKE_STATUS", L"0");
    // 让动作长时间不返回：收尾**绝不能**在这里等它。
    set_environment(L"DESKFEED_FAKE_DELAY_MS", L"1200");

    TaskbarSupervisor supervisor;
    std::string error;
    CHECK(supervisor.start(options_for(scope), true, error));
    supervisor.tick(monotonic_ms());
    CHECK(supervisor.action_in_flight());

    long long const before = monotonic_ms();
    supervisor.shutdown();
    long long const elapsed = monotonic_ms() - before;
    // 收尾只请求、不等待：这里必须立刻返回（留足调度余量，但远小于动作的 1200 ms）。
    CHECK(elapsed < 500);

    // 那次还在跑的动作结束后，收尾落的停止令牌必须被发出去，而不是随进程一起丢掉。
    std::vector<std::string> const expected{line_for("--status", scope), line_for("--stop", scope)};
    if (!pump_until_log(supervisor, log, expected, 15000)) {
        testing::report_failure("收尾的停止令牌丢了：" + join_log(read_log(log)));
    }
    settle(supervisor, 8000);
}

// ---------------------------------------------------------------------------
// 配置不成立时一次动作都不许发
// ---------------------------------------------------------------------------

TEST(supervisor_start_rejects_configurations_that_would_touch_other_processes) {
    TempDirectory dir;
    std::wstring const scope = L"sup-invalid";
    std::wstring const log = dir.join(L"actions.log");
    set_environment(L"DESKFEED_FAKE_LOG", log);
    set_environment(L"DESKFEED_FAKE_DELAY_MS", L"0");

    // 没有 master pid：负载要靠它绑定生产者、靠它退出自清理。
    {
        TaskbarSupervisor supervisor;
        TaskbarSupervisorOptions options = options_for(scope);
        options.master_pid = 0;
        std::string error;
        CHECK(!supervisor.start(options, true, error));
        CHECK(!supervisor.started());
    }
    // 退避与问询间隔必须在下限之上。
    {
        TaskbarSupervisor supervisor;
        TaskbarSupervisorOptions options = options_for(scope);
        options.retry_interval_ms = 10;
        std::string error;
        CHECK(!supervisor.start(options, true, error));
    }
    {
        TaskbarSupervisor supervisor;
        TaskbarSupervisorOptions options = options_for(scope);
        options.check_interval_ms = 10;
        std::string error;
        CHECK(!supervisor.start(options, true, error));
    }
    // 注入器不存在：能力不成立，绝不允许“先武装起来再默默失败”。
    {
        TaskbarSupervisor supervisor;
        TaskbarSupervisorOptions options = options_for(scope);
        options.injector_path = dir.join(L"no-such-injector.exe");
        std::string error;
        CHECK(!supervisor.start(options, true, error));
    }

    // 一路被拒绝，所以一份日志都不该有。
    CHECK_EQ(read_log(log).size(), static_cast<std::size_t>(0));
}

TEST(supervisor_armed_with_the_component_off_spawns_nothing) {
    TempDirectory dir;
    std::wstring const scope = L"sup-armed-off";
    std::wstring const log = dir.join(L"actions.log");
    set_environment(L"DESKFEED_FAKE_LOG", log);
    set_environment(L"DESKFEED_FAKE_STATUS", L"1");  // 就算是“未装载”也不该有人去装
    set_environment(L"DESKFEED_FAKE_DELAY_MS", L"0");

    TaskbarSupervisor supervisor;
    std::string error;
    // 用户上次就把组件关了：这次启动要武装好能力（注入器在不在），但**一次动作都不发**，
    // 尤其不能去 spawn 一次 `--stop`。这是 start() 收初始状态参数的理由。
    CHECK(supervisor.start(options_for(scope), false, error));
    CHECK(supervisor.started());
    CHECK(!supervisor.enabled());

    long long const deadline = monotonic_ms() + 800;
    while (monotonic_ms() < deadline) {
        supervisor.tick(monotonic_ms());
        ::Sleep(5);
    }
    CHECK(!supervisor.action_in_flight());
    CHECK_EQ(read_log(log).size(), static_cast<std::size_t>(0));
}

TEST(supervisor_not_started_never_spawns_anything) {
    TempDirectory dir;
    std::wstring const scope = L"sup-unarmed";
    std::wstring const log = dir.join(L"actions.log");
    set_environment(L"DESKFEED_FAKE_LOG", log);
    set_environment(L"DESKFEED_FAKE_STATUS", L"1");

    // 主进程在“没有显示邮箱”时**根本不武装**编排层（见 Master::start_display_face）。这条用例
    // 就是那个决定依赖的结构性保证：未武装的 tick 是空操作，任何配置都不会让它去 spawn。
    // 它比“武装成关”更强：不靠一个可能被写错的标志位，而是这条路径压根不存在。
    //
    // 覆盖面说明（不夸大）：一个从未武装的编排层有**两道**独立的门——tick / set_enabled 里的
    // `!started_`，以及默认就是 false 的 `enabled_` 目标状态。所以单独去掉 tick 那道门并不能
    // 让这条用例变红（还有 enabled_ 挡着）；能分辨它的是别的改动，例如去掉 shutdown() 里的
    // `!started_`（那会让收尾真的去 spawn 一次 `--stop`）。它是回归网，不是单点探针。
    TaskbarSupervisor supervisor;
    CHECK(!supervisor.started());

    long long const deadline = monotonic_ms() + 800;
    while (monotonic_ms() < deadline) {
        supervisor.tick(monotonic_ms());
        ::Sleep(5);
    }
    CHECK(!supervisor.action_in_flight());
    CHECK_EQ(read_log(log).size(), static_cast<std::size_t>(0));

    // set_enabled 对未武装的编排层也只能报“关”，一个字节都不许碰目标进程。
    CHECK(!supervisor.set_enabled(true));
    CHECK(!supervisor.enabled());
    for (int index = 0; index < 20; ++index) {
        supervisor.tick(monotonic_ms());
        ::Sleep(5);
    }
    CHECK_EQ(read_log(log).size(), static_cast<std::size_t>(0));

    // 收尾同样必须一次动作都不发。
    supervisor.shutdown();
    for (int index = 0; index < 20; ++index) {
        supervisor.tick(monotonic_ms());
        ::Sleep(5);
    }
    CHECK_EQ(read_log(log).size(), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------

int wmain(int argc, wchar_t** argv) {
    std::string filter;
    for (int index = 1; index < argc; ++index) {
        std::wstring const arg(argv[index]);
        if (arg == L"--filter" && index + 1 < argc) {
            filter = testing::to_display(std::wstring(argv[++index]));
        } else if (arg == L"--help" || arg == L"-h") {
            std::printf("deskfeed-supervisor-tests.exe [--filter <substring>]\n");
            return 0;
        }
    }

    int passed = 0;
    int failed = 0;
    for (testing::Case const& test : testing::cases()) {
        std::string const name(test.name);
        if (!filter.empty() && name.find(filter) == std::string::npos) {
            continue;
        }
        try {
            test.body();
            std::printf("ok   %s\n", test.name);
            ++passed;
        } catch (testing::Failure const& failure) {
            std::printf("FAIL %s: %s\n", test.name, failure.message.c_str());
            ++failed;
        } catch (std::exception const& error) {
            std::printf("FAIL %s: unexpected exception: %s\n", test.name, error.what());
            ++failed;
        } catch (...) {
            std::printf("FAIL %s: unknown exception\n", test.name);
            ++failed;
        }
    }
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
