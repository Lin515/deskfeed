#include "payload/host.h"

#include "core/utf8.h"
#include "display/frame.h"
#include "display/mailbox.h"
#include "display/reading.h"
#include "payload/action_worker.h"
#include "payload/feed.h"
#include "payload/hook_post.h"
#include "payload/reg_hook.h"
#include "payload/retire_gates.h"
#include "payload/taskbar_xaml.h"
#include "payload/xaml_tap.h"
#include "platform/log.h"
#include "platform/paths.h"
#include "platform/win32_util.h"

#include <windows.h>

// 当前线程上建调度队列要用原始入口：SDK 的 C++/WinRT 投影只暴露 CreateOnDedicatedThread。
#include <dispatcherqueue.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>

#include <atomic>
#include <exception>
#include <string>

namespace deskfeed::payload {
namespace {

std::atomic<bool> g_stop_requested{false};
DWORD g_thread_id = 0;
HANDLE g_cleanup_event = nullptr;
HANDLE g_payload_thread = nullptr;
std::atomic<bool> g_thread_published{false};
std::atomic<DWORD> g_unload_safe{0};
// 状态 ABI（见 src/payload/status_pod.h）要的东西。**没有任何跨线程可见的裸指针指向栈上对象**：
// 生产者身份只以"我们自己 dup 的一份进程句柄 + pid + 创建时间 + scope + 代次"这个固定快照存在，
// 读写都在同一把 SRWLOCK 里成组进行。查询方只能在短锁里 dup 一份句柄，拿出去之后再对它做真实
// 的 `WaitForSingleObject(.,0)` / `GetProcessId` / `GetProcessTimes`。
SRWLOCK g_status_lock = SRWLOCK_INIT;
HANDLE g_status_producer = nullptr;  // 本模块持有的一份 dup；只在锁里替换/关闭
std::uint32_t g_status_attached = 0;
std::uint32_t g_status_started = 0;
std::uint32_t g_status_generation = 0;
wchar_t g_status_scope[kPayloadScopeCapacity]{};
std::atomic<bool> g_mailbox_attached{false};
std::atomic<unsigned> g_feed_failures{0};
winrt::Windows::System::DispatcherQueueController g_dispatcher_controller{nullptr};

// ---- 回收门（retirement gates）--------------------------------------------------------------
//
// 每个门位都是**某条现成 API 的真实结果**，只可能由 false 变 true；没有任何一步能把已经记下的
// 失败抹掉。`unload_safe` 是这些门位的纯派生量（全过 && 没崩），不是另一次赋值——所以"最后一次
// 调用把 0 覆盖成 1"这种结论结构上不可能再发生。
//
//   worker      `action_worker_stop` 真的等到线程句柄触发
//   xaml        `taskbar_xaml_detach`（它内部已经 AND 了菜单/悬停/摘块/钩子静止证明）
//   tap         `xaml_tap_stop` 的站点归还与 SetSite 排空证据
//   iat         `reg_hook_restore`（已接受的残余窗口；恢复失败只影响它自己）
//   consumer    邮箱消费者状态已经消失（identity 快照清空、mailbox_attached=false）
//   thread_done 负载线程已经走完全部模块侧工作（只剩 `ret`）
//   crashed     C++/SEH 故障，**粘性**
// 回收门与 `unload_safe` 的推导都在 `payload/retire_gates.h`：负载实现与离线用例共用同一份代码。
RetirementGates g_gates;
// 同一时刻只允许一个回收驱动者（负载线程的正常收尾，或 `DeskFeedStop` 的重试）。它只是"谁是驱动
// 者"的仲裁，**绝不**在持有它的时候做等待/ping。
std::atomic<bool> g_retire_running{false};

std::uint32_t refresh_unload_safe() {
    // 唯一的推导处：门位集合的纯函数（`payload/retire_gates.h`，离线用例咬的就是这一份）。
    std::uint32_t const value = g_gates.derived_unload_safe();
    g_unload_safe.store(value);
    return value;
}

void append_detail(std::wstring& detail, std::wstring const& text) {
    if (text.empty()) {
        return;
    }
    try {
        if (!detail.empty()) {
            detail += L"; ";
        }
        detail += text;
    } catch (...) {
    }
}

// 清掉 identity 快照并把 dup 句柄还回去。幂等：正常收尾、异常展开、`safe_finalize` 都调它。
void clear_status_producer() {
    HANDLE stale = nullptr;
    ::AcquireSRWLockExclusive(&g_status_lock);
    stale = g_status_producer;
    g_status_producer = nullptr;
    g_status_attached = 0;
    ::ReleaseSRWLockExclusive(&g_status_lock);
    g_mailbox_attached.store(false);
    if (stale != nullptr) {
        ::CloseHandle(stale);
    }
}

// 负载线程 attach 成功之后登记身份：dup 一份**独立**的生产者进程句柄。之后任何查询都不再需要
// 那个栈上的 `DisplayFrameConsumer` 还活着。
void publish_status_producer(HANDLE producer) {
    HANDLE duplicate = nullptr;
    if (producer != nullptr) {
        if (::DuplicateHandle(::GetCurrentProcess(), producer, ::GetCurrentProcess(), &duplicate, 0,
                              FALSE, 0) == FALSE) {
            duplicate = nullptr;
        }
    }
    ::AcquireSRWLockExclusive(&g_status_lock);
    HANDLE const stale = g_status_producer;
    g_status_producer = duplicate;
    g_status_attached = duplicate != nullptr ? 1u : 0u;
    ::ReleaseSRWLockExclusive(&g_status_lock);
    if (stale != nullptr) {
        ::CloseHandle(stale);
    }
}

bool create_dispatcher_queue() {
    DispatcherQueueOptions options{};
    options.dwSize = sizeof(options);
    options.threadType = DQTYPE_THREAD_CURRENT;
    options.apartmentType = DQTAT_COM_NONE;

    winrt::com_ptr<ABI::Windows::System::IDispatcherQueueController> raw;
    HRESULT const result = ::CreateDispatcherQueueController(
        options, reinterpret_cast<PDISPATCHERQUEUECONTROLLER*>(raw.put()));
    if (FAILED(result)) {
        log_format(L"CreateDispatcherQueueController -> %s", hresult_text(result).c_str());
        return false;
    }
    g_dispatcher_controller =
        winrt::Windows::System::DispatcherQueueController{raw.detach(), winrt::take_ownership_from_abi};
    log_line(L"dispatcher queue created on current thread");
    return true;
}

void run_message_loop(DisplayFrameConsumer const& consumer) {
    HANDLE wait_handles[2] = {consumer.ready_event(), consumer.producer_process()};
    MSG message{};
    while (!g_stop_requested.load()) {
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                return;
            }
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        // 用 MsgWaitForMultipleObjects 让出（而不是阻塞在 GetMessage）：本线程的调度队列才有机会
        // 跑它的工作项，XAML 的 composition 提交就是这样送出去的。
        DWORD const wait = ::MsgWaitForMultipleObjectsEx(2, wait_handles, 250, QS_ALLINPUT,
                                                         MWMO_INPUTAVAILABLE);
        if (wait == WAIT_OBJECT_0 + 1) {
            // 生产者进程句柄触发 = 主进程已经退出（正常或崩溃）。没有来源的读数必须立刻消失。
            log_line(L"the display mailbox producer is gone; removing the taskbar block");
            break;
        }
        // ready 事件（WAIT_OBJECT_0）与超时都在下面这一轮里处理：统一读一次邮箱。
        break;
    }
}

// 收尾日志与清理事件都可能在"已经有一次异常在路上"的时候才跑，所以每一步都要自己吞掉异常：
// 不能因为记账失败就把进程带走。
void safe_log_line(wchar_t const* text) {
    try {
        log_line(text);
    } catch (...) {
    }
}

void safe_log_wide(wchar_t const* prefix, std::wstring const& text) {
    try {
        log_format(L"%s%s", prefix, text.c_str());
    } catch (...) {
    }
}

// 收尾。它**只能反映**门位集合与 crash 粘性，没有任何参数能让它把失败改写成成功：
// `refresh_unload_safe()` 是唯一写 `g_unload_safe` 的地方，它算的是全部门位的与。
void safe_finalize(bool crashed) {
    try {
        // identity 快照必须在这里显式清：MSVC 的 /EHsc 下 SEH `__except` 不保证展开路径上的 C++
        // 析构函数会跑，所以不能只靠局部 `DisplayFrameConsumer` 的析构。
        clear_status_producer();

        if (hook_post_dangling()) {
            log_line(L"a hook is still dangling; the module must stay loaded");
        }
        if (crashed) {
            g_gates.crashed.store(true);
        }
        refresh_unload_safe();
        log_format(L"payload stopped (unload safe: %s) gates[worker=%d xaml=%d tap=%d iat=%d "
                   L"consumer=%d thread=%d crashed=%d] dangling=%d",
                   g_unload_safe.load() == 1 ? L"yes" : L"no", g_gates.worker.load() ? 1 : 0,
                   g_gates.xaml.load() ? 1 : 0, g_gates.tap.load() ? 1 : 0, g_gates.iat.load() ? 1 : 0,
                   g_gates.consumer.load() ? 1 : 0, g_gates.thread_done.load() ? 1 : 0,
                   g_gates.crashed.load() ? 1 : 0, hook_post_dangling() ? 1 : 0);
    } catch (...) {
        // 记账失败时按"未退休"处理：宁可多留一份映射，也不能报告可卸载。
        g_gates.crashed.store(true);
        refresh_unload_safe();
    }

    // 调度队列刻意**不**经 ShutdownQueueAsync 关：它的完成回调会回到本模块，上一次实测直接把
    // explorer 打崩。丢掉控制器就够了，队列随线程消失。
    try {
        g_dispatcher_controller = nullptr;
    } catch (...) {
    }

    if (g_cleanup_event != nullptr) {
        // 清理事件只是**观测**信号，不是卸载门；在模块代码全部退出之前置位。
        ::SetEvent(g_cleanup_event);
    }
    try {
        log_close();
    } catch (...) {
    }
}

// SEH 边界的故障记录。只允许引用参数，不能有需要展开的局部对象；这里只写两个 out 参数，
// `GetModuleHandleExW` 不分配，日志留给调用方。
void record_fault(EXCEPTION_POINTERS* info, unsigned long& code, unsigned long long& offset) {
    code = 0;
    offset = 0;
    if (info == nullptr || info->ExceptionRecord == nullptr) {
        return;
    }
    code = static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode);
    void* const address = info->ExceptionRecord->ExceptionAddress;
    HMODULE module = nullptr;
    if (address != nullptr &&
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(address), &module) != FALSE) {
        offset = static_cast<unsigned long long>(reinterpret_cast<unsigned char*>(address) -
                                                 reinterpret_cast<unsigned char*>(module));
    }
}

// SEH 一层：在 C++ 对象全部析构之后进入，只调用一个不在本函数里展开任何东西的函数指针。
void run_core_seh(void (*core)(HINSTANCE, std::wstring const&), HINSTANCE module_instance,
                  std::wstring const& scope, unsigned long& fault_code,
                  unsigned long long& fault_offset) {
    __try {
        core(module_instance, scope);
    } __except (record_fault(GetExceptionInformation(), fault_code, fault_offset),
                EXCEPTION_EXECUTE_HANDLER) {
    }
}

// 回收驱动者：把还没拿到证明的门各走一遍，返回 true 表示全部门位都拿到了。
//
// 三条纪律：
//   * **唯一驱动者**：`g_retire_running` 的 CAS 决定谁在驱动；竞争者立刻返回失败，绝不排队，也
//     绝不让两个线程同时进 `taskbar_xaml_detach` / `xaml_tap_stop`（它们内部都有"只还一次"的状态）。
//   * **仲裁位里不做任何等待**：CAS 之后立刻进入无锁状态，每一步的真实等待（线程句柄、ping）都
//     发生在没有锁的情况下；否则收尾 ping 会变成死锁而不是证明。
//   * **不 undo**：某一步失败只让对应的门位保持 false，下一次重试再试；已经置位的门不重跑。
bool retire_gates(unsigned timeout_ms, bool mark_thread_done, std::wstring& detail) {
    bool expected = false;
    if (!g_retire_running.compare_exchange_strong(expected, true)) {
        detail = L"另一次回收正在进行";
        return false;
    }
    struct release_running {
        ~release_running() { g_retire_running.store(false); }
    } release;

    std::uint64_t const deadline =
        ::GetTickCount64() + (timeout_ms == 0 ? 1u : timeout_ms);
    auto remaining = [deadline]() -> unsigned {
        std::uint64_t const now = ::GetTickCount64();
        if (now >= deadline) {
            return 1;
        }
        std::uint64_t const left = deadline - now;
        return left > 60000 ? 60000u : static_cast<unsigned>(left);
    };

    if (!g_gates.worker.load()) {
        if (action_worker_stop(remaining())) {
            g_gates.worker.store(true);
        } else {
            append_detail(detail, L"动作工作线程没有在期限内退出");
        }
    }
    if (!g_gates.xaml.load()) {
        std::wstring step_detail;
        if (taskbar_xaml_detach(remaining(), step_detail)) {
            g_gates.xaml.store(true);
        } else {
            append_detail(detail, step_detail);
        }
    }
    if (!g_gates.tap.load()) {
        std::wstring step_detail;
        if (xaml_tap_stop(remaining(), step_detail)) {
            g_gates.tap.store(true);
        } else {
            append_detail(detail, step_detail);
        }
    }
    if (!g_gates.iat.load()) {
        std::wstring step_detail;
        if (reg_hook_restore(step_detail)) {
            g_gates.iat.store(true);
        } else {
            append_detail(detail, L"导入表没有恢复: " + step_detail);
        }
    }
    if (!g_gates.consumer.load()) {
        // 消费者**状态**证据：identity 快照清空、mailbox_attached=false，模块里再没有可达的消费者。
        // 消费者自己的句柄归负载线程的局部对象，正常路径上由它的析构成对归还；本线程的
        // `thread_done` 门位在它析构之后才置位，所以"可卸载"永远晚于句柄归还。
        clear_status_producer();
        g_gates.consumer.store(true);
    }
    if (mark_thread_done) {
        g_gates.thread_done.store(true);
    }

    bool const safe = refresh_unload_safe() == 1;
    if (!safe && detail.empty()) {
        detail = L"还有回收门没有拿到证明";
    }
    return safe;
}

// 负载线程真正做的事。收尾（清理事件、卸载门、日志收尾）由外层 `payload_run` 在边界之外负责，
// 所以这里可以**提前返回**，任何一条路径都不会漏掉收尾。
void payload_run_core(HINSTANCE module_instance, std::wstring const& scope) {
    (void)module_instance;

    // 与 explorer 的 DPI 感知对齐，免得子窗口被虚拟化到错误的像素上。
    ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 调度队列必须先于 WinRT 单元：compositor 绑定创建时当前线程的那个队列。
    create_dispatcher_queue();
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
    } catch (winrt::hresult_error const& failure) {
        log_format(L"init_apartment failed: %s", hresult_text(failure.code()).c_str());
    }

    std::string attach_error;
    DisplayFrameConsumer consumer;
    if (!consumer.attach(scope, attach_error)) {
        // 生产者已经不在了也算失败：据此立刻清理，不留旧读数。
        log_format(L"display mailbox: not attached: %S", attach_error.c_str());
        return;
    }
    g_mailbox_attached.store(true);
    // 身份登记：dup 一份**独立**的生产者进程句柄进 SRW 快照。这里绝不发布任何指向 `consumer`
    // 的指针——它是本函数的栈上对象，查询线程不可能安全地解引用它。
    publish_status_producer(consumer.producer_process());
    log_line(L"display mailbox attached");

    // 第一份读数：主进程启动时就会发一帧，所以任务栏块一出现就是真实内容，不是占位。
    Frame first;
    if (consumer.read(first)) {
        taskbar_xaml_set_reading(reading_from_frame(first));
    } else {
        taskbar_xaml_set_reading(reading_from_frame(Frame{}));
    }

    std::wstring worker_error;
    if (!action_worker_start(scope, worker_error)) {
        log_format(L"action worker: %s", worker_error.c_str());
    }

    std::wstring attach_xaml_error;
    if (taskbar_xaml_attach(scope, attach_xaml_error)) {
        log_line(L"taskbar reading block applied");
    } else {
        log_format(L"taskbar xaml attach failed: %s", attach_xaml_error.c_str());
    }

    unsigned failures = 0;
    bool have_last = consumer.read(first);
    while (!g_stop_requested.load()) {
        run_message_loop(consumer);
        if (g_stop_requested.load()) {
            break;
        }
        if (!consumer.producer_running()) {
            // 生产者已死：拆块，不保留旧读数。
            log_line(L"the display mailbox producer died; the reading has no source any more");
            taskbar_xaml_request_update(ReadingContent{});
            break;
        }
        Frame frame;
        bool const read_ok = consumer.read(frame);
        FeedInput input;
        input.read_ok = read_ok;
        input.closed = read_ok && frame.closed;
        input.owner_alive = consumer.producer_running();
        input.have_last_frame = have_last;
        input.failures = failures;
        FeedOutcome const decision = feed_decide(input);
        if (decision == FeedOutcome::Apply) {
            have_last = true;
            failures = 0;
            g_feed_failures.store(0);
            taskbar_xaml_request_update(reading_from_frame(frame));
        } else if (decision == FeedOutcome::Keep) {
            ++failures;
            g_feed_failures.store(failures);
        } else {
            // Remove 有三种不同原因，日志必须分得开：验收要能看出"生产者正常收尾"、"生产者没了"与
            // "帧坏了又没有可沿用的旧帧"。
            failures = 0;
            have_last = false;
            g_feed_failures.store(0);
            if (input.closed) {
                log_line(L"the display mailbox closed by the producer; removing the taskbar block");
            } else if (!input.owner_alive) {
                log_line(L"the producer is gone; dropping the reading and removing the taskbar block");
            } else {
                log_line(L"the reading is dropped (no complete frame and no usable last frame)");
            }
            taskbar_xaml_request_update(ReadingContent{});
            break;
        }
    }

    // 拆除在 `payload_run` 的边界之外统一做（`retire_gates`）：那里覆盖了正常返回、提前返回、
    // C++ 异常与 SEH 故障全部退出路径。这里只把消费者的句柄成对还回去。
    consumer.detach();
    g_mailbox_attached.store(false);
}

}  // namespace

// 由 `DllMain` 调用：**只**做 kernel32 调用与栈上的名字拼接。
//
// 纪律（直接来自"这个模块要在 explorer 里活着"）：DllMain 跑在**加载器锁**里，这里不许做文件 I/O、
// 不许等待、不许 user32/COM/Registry、不许查 SID、不许分配 —— 所以名字在栈上的定长缓冲里拼，不用
// `std::wstring`。"上一次实验留下的同名对象已经置位"这种交接清理要等一次事件、还要写日志，两件事
// 都在 `payload_run`（负载线程、加载器锁之外）里做。
void payload_prepare() {
    if (g_cleanup_event != nullptr) {
        return;
    }

    constexpr std::size_t kNameCapacity = 64;
    wchar_t const prefix[] = L"Local\\DeskFeed-payload-cleanup-";
    wchar_t name[kNameCapacity]{};
    std::size_t length = 0;
    while (prefix[length] != L'\0' && length + 12 < kNameCapacity) {
        name[length] = prefix[length];
        ++length;
    }

    // pid：十进制、无前导零。
    wchar_t digits[10]{};
    std::size_t digit_count = 0;
    unsigned long remaining = ::GetCurrentProcessId();
    do {
        digits[digit_count++] = static_cast<wchar_t>(L'0' + (remaining % 10u));
        remaining /= 10u;
    } while (remaining != 0 && digit_count < sizeof(digits) / sizeof(digits[0]));
    for (std::size_t index = 0; index < digit_count && length + 1 < kNameCapacity; ++index) {
        name[length++] = digits[digit_count - 1 - index];
    }
    name[length] = L'\0';

    g_cleanup_event = ::CreateEventW(nullptr, TRUE, FALSE, name);
}

HANDLE payload_cleanup_event() { return g_cleanup_event; }

HANDLE payload_thread_handle() { return g_payload_thread; }

// 由 `DeskFeedStart` 在**线程开始跑之前**登记它刚创建的线程句柄（配合 `CREATE_SUSPENDED`）。
// 这条登记是"立即 DeskFeedStop"能拿到真实句柄的唯一保证：以前句柄是在负载线程里
// `DuplicateHandle` 出来的，线程还没被调度时 `payload_thread_handle()` 返回 nullptr，
// `DeskFeedStop` 就会在一条本模块的线程已经在跑的情况下报告"可以卸载"。
bool payload_publish_thread(HANDLE thread) {
    if (thread == nullptr) {
        return false;
    }
    bool expected = false;
    if (!g_thread_published.compare_exchange_strong(expected, true)) {
        return false;
    }
    g_payload_thread = thread;
    g_thread_id = ::GetThreadId(thread);
    return true;
}

DWORD payload_unload_safe() { return g_unload_safe.load(); }

void payload_request_stop() {
    g_stop_requested.store(true);
    DWORD const thread_id = g_thread_id;
    if (thread_id == 0) {
        return;
    }
    // 线程的消息队列在它第一次建窗口或调 PeekMessage 时才存在；刚启动就收到 stop 时要重试几下。
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (::PostThreadMessageW(thread_id, WM_QUIT, 0, 0) != FALSE) {
            return;
        }
        ::Sleep(25);
    }
    log_line(L"PostThreadMessage(WM_QUIT) never succeeded");
}

// 线程入口的**最外层**边界。里面每一步都可能抛（WinRT 的 `hresult_error`、`std::bad_alloc`、
// 字符串构造……），异常穿出线程入口就是未处理异常 = explorer 终止。所以分三道：
//   1. C++ 一层：`catch (...)` 吃掉 C++ 异常；
//   2. SEH 一层：在 C++ 对象全部析构之后才进叶子（MSVC 的 `/EHsc` 下 `catch (...)` 不接管访问
//      违例等非 C++ 异常，所以这是独立的一道，不是重复）；
//   3. 收尾记账（清理事件、卸载门、日志收尾）在**边界之外**做，且自己吞异常——即使前两道都失败，
//      也必须留下"未退休"的结论与观测信号。
void payload_run(HINSTANCE module_instance, std::wstring const& scope) {
    bool crashed = false;
    g_unload_safe.store(0);
    try {
        g_thread_id = ::GetCurrentThreadId();
        // 句柄由 `DeskFeedStart` 在 Resume 之前登记（见 `payload_publish_thread`）。只有那条路径
        // 没走过时（例如自测宿主直接调 `payload_run`）才自己复制一份。
        if (!g_thread_published.load()) {
            HANDLE duplicate = nullptr;
            if (::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(),
                                  &duplicate, SYNCHRONIZE, FALSE, 0) != FALSE) {
                if (payload_publish_thread(duplicate)) {
                    // 登记的句柄归负载所有，模块卸载前不关。
                } else {
                    ::CloseHandle(duplicate);
                }
            }
        }
        log_open(default_log_path(L"deskfeed-payload.log"), false);
        log_format(L"payload thread started: pid=%lu scope=\"%s\"", ::GetCurrentProcessId(),
                   scope.c_str());

        // DllMain 只把清理事件建出来（不能等待、不能写日志）。"上一次实验留下的同名对象已经置位"
        // 这种交接清理放在这里：加载器锁之外，等一次事件与写日志都安全。不清会让这一次的 stop
        // 立刻返回，交接就错位了。
        if (HANDLE const cleanup = payload_cleanup_event();
            cleanup != nullptr && ::WaitForSingleObject(cleanup, 0) == WAIT_OBJECT_0) {
            log_line(L"warning: the cleanup event was already signalled; resetting it");
            ::ResetEvent(cleanup);
        }

        unsigned long fault_code = 0;
        unsigned long long fault_offset = 0;
        run_core_seh(&payload_run_core, module_instance, scope, fault_code, fault_offset);
        if (fault_code != 0) {
            crashed = true;
            safe_log_wide(L"", L"payload thread fault 0x" + hex_text(fault_code) +
                                   L"（相对本模块 +0x" + hex_text(fault_offset) + L"）");
        }
    } catch (winrt::hresult_error const& failure) {
        crashed = true;
        safe_log_wide(L"payload thread stopped on a hresult_error: ", hresult_text(failure.code()));
    } catch (std::exception const& failure) {
        crashed = true;
        safe_log_wide(L"payload thread stopped on an exception: ",
                      deskfeed::wide_from_utf8(failure.what()));
    } catch (...) {
        crashed = true;
        safe_log_line(L"payload thread stopped on an unknown exception");
    }

    // 回收门在**边界之外**统一走一遍：正常返回、提前返回、C++ 异常与 SEH 故障四条退出路径都在
    // 这里汇合。某一步失败只让对应的门位保持 false，结论由门位集合派生。
    std::wstring retire_detail;
    if (!retire_gates(5000, /*mark_thread_done=*/false, retire_detail)) {
        safe_log_wide(L"retirement is incomplete: ", retire_detail);
    }
    // 本线程已经走完模块侧全部工作，只剩 `ret`：这是最后一道模块侧门位。真正的"线程已终止"
    // 证明由 `DeskFeedStop` 对自己的线程句柄等待得到，两者同时成立才算可以卸载。
    g_gates.thread_done.store(true);
    refresh_unload_safe();

    safe_finalize(crashed);

    // 返回之后由线程入口回到 kernel32：从那一刻起本模块不再有属于这条线程的指令。
}

// 状态 ABI（见 src/payload/status_pod.h）。由导出的 `DeskFeedGetStatus` 调用。
//
// 纪律：
//   * `thread_alive` 的判据是**线程句柄没触发**，不是任何标志位。
//   * identity（生产者存活/PID/创建时间）来自 SRW 快照里我们自己 dup 的那份句柄：查询线程在短锁里
//     dup 一份拿出去，再对它做真实等待与查询。所以它既不可能踩到负载线程的栈上对象，也不可能
//     正踩着一个被并发关掉的句柄。
//   * 整段是一个独立 SEH 叶：不分配、不记日志、不碰 UI、不碰任何 COM 对象（`xaml_block_present`
//     读的是任务栏线程维护的那个原子观测值）。
bool payload_get_status(PayloadStatusPod& pod) {
    HANDLE const thread = g_payload_thread;
    pod.thread_alive =
        (thread != nullptr && ::WaitForSingleObject(thread, 0) == WAIT_TIMEOUT) ? 1u : 0u;
    pod.unload_safe = g_unload_safe.load();
    pod.dangling = hook_post_dangling() ? 1u : 0u;
    pod.mailbox_attached = g_mailbox_attached.load() ? 1u : 0u;
    pod.feed_failures = g_feed_failures.load();
    pod.xaml_block_present = taskbar_xaml_block_visible() ? 1u : 0u;
    // reserved：负载没有暴露帧代次的只读入口，这里不编造数据；工具不得用 0 判断"是否活跃"。
    pod.frame_generation = 0;

    __try {
        HANDLE producer = nullptr;
        ::AcquireSRWLockShared(&g_status_lock);
        pod.started = g_status_started;
        if (g_status_producer != nullptr) {
            ::DuplicateHandle(::GetCurrentProcess(), g_status_producer, ::GetCurrentProcess(),
                              &producer, 0, FALSE, 0);
        }
        // scope 与 identity 在**同一把锁**里读走，保证它们属于同一代。
        for (std::size_t index = 0; index < kPayloadScopeCapacity; ++index) {
            pod.scope[index] = g_status_scope[index];
        }
        pod.scope[kPayloadScopeCapacity - 1] = L'\0';
        ::ReleaseSRWLockShared(&g_status_lock);

        if (producer != nullptr) {
            pod.producer_alive = ::WaitForSingleObject(producer, 0) == WAIT_TIMEOUT ? 1u : 0u;
            DWORD const pid = ::GetProcessId(producer);
            pod.producer_pid_low = static_cast<std::uint32_t>(pid);
            pod.producer_pid_high = 0;
            FILETIME created{};
            FILETIME exited{};
            FILETIME kernel{};
            FILETIME user{};
            if (::GetProcessTimes(producer, &created, &exited, &kernel, &user) != FALSE) {
                ULARGE_INTEGER stamp{};
                stamp.LowPart = created.dwLowDateTime;
                stamp.HighPart = created.dwHighDateTime;
                pod.producer_start_time = stamp.QuadPart;
            }
            ::CloseHandle(producer);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void payload_publish_scope(std::wstring const& scope) {
    // 开新的一代：上一代的身份快照、门位与结论全部作废。这里只做定长拷贝与整数赋值。
    clear_status_producer();
    g_gates.worker.store(false);
    g_gates.xaml.store(false);
    g_gates.tap.store(false);
    g_gates.iat.store(false);
    g_gates.consumer.store(false);
    g_gates.thread_done.store(false);
    g_gates.crashed.store(false);
    g_feed_failures.store(0);

    std::size_t const length =
        scope.size() < kPayloadScopeCapacity - 1 ? scope.size() : kPayloadScopeCapacity - 1;
    ::AcquireSRWLockExclusive(&g_status_lock);
    for (std::size_t index = 0; index < length; ++index) {
        g_status_scope[index] = scope[index];
    }
    g_status_scope[length] = L'\0';
    g_status_started = 1;
    ++g_status_generation;
    ::ReleaseSRWLockExclusive(&g_status_lock);

    refresh_unload_safe();
}

void payload_mark_start_rejected() {
    ::AcquireSRWLockExclusive(&g_status_lock);
    g_status_started = 0;
    ::ReleaseSRWLockExclusive(&g_status_lock);
    log_line(L"Start was rejected after the thread handle was published; the module stays loaded");
}

bool payload_retire_after_thread(unsigned timeout_ms, std::wstring& detail) {
    // 调用方（`DeskFeedStop`）已经自己等过真实线程句柄：线程确实终止或从未起过，所以 `thread_done`
    // 这条门位现在可以置上，其余没过的门由 `retire_gates` 补试。
    return retire_gates(timeout_ms, /*mark_thread_done=*/true, detail);
}

}  // namespace deskfeed::payload
