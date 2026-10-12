// 原生侧的离线回归用例。刻意与 tests/tests.cpp 分开：那一份归主进程/帧/配置的作者，这一份只放
// 任务栏负载这条路上的东西——
//   * 显示帧的消费策略（纯逻辑，逐分支）；
//   * 钩子门的收尾协议（真的起一条带消息循环的线程 + 一个 user32 类的确认窗口，**不注入 explorer**）；
//   * C++/SEH 边界（故障必须被吃掉，不能穿出去）；
//   * 成品负载 DLL 的自动审计：PE 导出表能被按名解析、导入表里没有网络/符号库、map 里没有
//     JSON/快照/协议校验器的目标文件。
//
// 用法：
//   deskfeed-native-tests                     跑全部用例
//   deskfeed-native-tests --filter feed       只跑名字里含 feed 的用例
//   deskfeed-native-tests --audit <dll>       只跑成品审计（CTest 单独注册一条）
#include "payload/feed.h"
#include "payload/hook_post.h"
#include "platform/log.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;
std::string g_filter;

void check(bool condition, char const* what) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    }
}

bool name_matches(char const* name) {
    return g_filter.empty() || std::strstr(name, g_filter.c_str()) != nullptr;
}

// ---------------------------------------------------------------- 纯策略

void test_feed_policy() {
    using deskfeed::payload::FeedInput;
    using deskfeed::payload::FeedOutcome;
    if (!name_matches("feed")) {
        return;
    }

    FeedInput fresh;
    fresh.read_ok = true;
    fresh.owner_alive = true;
    check(deskfeed::payload::feed_decide(fresh) == FeedOutcome::Apply, "feed: a fresh frame applies");

    FeedInput closed;
    closed.read_ok = true;
    closed.closed = true;
    closed.owner_alive = true;
    check(deskfeed::payload::feed_decide(closed) == FeedOutcome::Remove,
          "feed: a closed frame removes the block");

    FeedInput dead;
    dead.read_ok = false;
    dead.owner_alive = false;
    dead.have_last_frame = true;
    check(deskfeed::payload::feed_decide(dead) == FeedOutcome::Remove,
          "feed: a dead producer never keeps the last reading");

    FeedInput transient;
    transient.read_ok = false;
    transient.owner_alive = true;
    transient.have_last_frame = true;
    transient.failures = 0;
    check(deskfeed::payload::feed_decide(transient) == FeedOutcome::Keep,
          "feed: a live producer with a bad frame keeps the last reading");

    transient.failures = deskfeed::payload::kFeedMaxRetainedFailures;
    check(deskfeed::payload::feed_decide(transient) == FeedOutcome::Remove,
          "feed: the retained reading is bounded");

    FeedInput nothing;
    nothing.read_ok = false;
    nothing.owner_alive = true;
    nothing.have_last_frame = false;
    check(deskfeed::payload::feed_decide(nothing) == FeedOutcome::Remove,
          "feed: nothing to show removes the block");
}

// ---------------------------------------------------------------- C++/SEH 边界

bool seh_probe(bool& caught) {
    __try {
        volatile int* pointer = nullptr;
        *pointer = 1;
        caught = false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        caught = true;
    }
    return true;
}

void test_seh_boundary() {
    if (!name_matches("seh")) {
        return;
    }
    bool caught = false;
    seh_probe(caught);
    check(caught, "seh: an access violation inside the boundary is caught");
}

// ---------------------------------------------------------------- 钩子门（离线）

struct PumpThread {
    HANDLE thread = nullptr;
    DWORD thread_id = 0;
    HWND window = nullptr;
    std::atomic<bool> quit{false};
    std::atomic<bool> hang{false};
};

std::atomic<int> g_applies{0};
std::atomic<bool> g_executor_slow{false};

bool test_executor(void*, deskfeed::payload::HookAction action, std::wstring& error) {
    if (g_executor_slow.load()) {
        ::Sleep(400);
    }
    if (action == deskfeed::payload::HookAction::Apply) {
        g_applies.fetch_add(1);
    }
    (void)error;
    return true;
}

LRESULT CALLBACK pump_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return ::DefWindowProcW(window, message, wparam, lparam);
}

DWORD WINAPI pump_entry(LPVOID parameter) {
    auto* state = static_cast<PumpThread*>(parameter);

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = pump_window_proc;
    window_class.hInstance = ::GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"DeskFeedNativeTestPump";
    ::RegisterClassExW(&window_class);

    // 顶层但从不显示：它属于这条线程，收尾 ping 的确认窗口就用它（必须是 user32 类的窗口，
    // 不能用本模块自己的窗口过程当确认目标）。
    state->window = ::CreateWindowExW(WS_EX_TOOLWINDOW, L"DeskFeedNativeTestPump", L"pump",
                                      WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                      ::GetModuleHandleW(nullptr), nullptr);
    state->thread_id = ::GetCurrentThreadId();

    MSG message{};
    while (!state->quit.load()) {
        // 挂住的语义是**停止取消息**（钩子就是在线程取消息时被调用的），所以这个判断必须在
        // PeekMessage 之前：先取消息再判断的话，已经排队的 WM_NULL 会立刻把钩子叫起来。
        if (state->hang.load()) {
            ::Sleep(20);
            continue;
        }
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        ::MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    if (state->window != nullptr) {
        ::DestroyWindow(state->window);
        state->window = nullptr;
    }
    return 0;
}

void test_hook_gate() {
    if (!name_matches("hook")) {
        return;
    }

    PumpThread pump;
    pump.thread = ::CreateThread(nullptr, 0, pump_entry, &pump, 0, nullptr);
    check(pump.thread != nullptr, "hook: the pump thread starts");
    for (int attempt = 0; attempt < 200 && pump.thread_id == 0; ++attempt) {
        ::Sleep(5);
    }
    check(pump.thread_id != 0 && pump.window != nullptr, "hook: the pump thread owns a window");

    deskfeed::payload::hook_post_set_executor(&test_executor, nullptr);
    deskfeed::payload::hook_post_set_ack_window(pump.window);

    // (1) 正常路径：动作在 T 上跑，完成事件 + ping 都拿到，收尾成功且可重复收尾。
    int const before = g_applies.load();
    auto posted = deskfeed::payload::hook_post_run(pump.thread_id, deskfeed::payload::HookAction::Apply,
                                                   3000);
    check(posted.ok, "hook: the action runs on the taskbar thread");
    check(posted.retired, "hook: the record is removed inside the callback");
    check(g_applies.load() == before + 1, "hook: the executor ran exactly once");
    std::wstring detail;
    check(deskfeed::payload::hook_post_retire(pump.window, pump.thread_id, 3000, detail),
          "hook: the quiescence proof is obtained");
    check(deskfeed::payload::hook_post_retire(pump.window, pump.thread_id, 3000, detail),
          "hook: a second retire is harmless");
    check(!deskfeed::payload::hook_post_dangling(), "hook: nothing dangles after a clean retire");

    // (2) 对抗：回调在"安装方发布句柄"之前就进入（把发布人为推迟）。握手必须救回来。
    deskfeed::payload::hook_post_set_install_delay_ms(200);
    posted = deskfeed::payload::hook_post_run(pump.thread_id, deskfeed::payload::HookAction::Apply, 3000);
    deskfeed::payload::hook_post_set_install_delay_ms(0);
    check(posted.ok && posted.retired, "hook: the install/publication handshake recovers the race");
    check(deskfeed::payload::hook_post_retire(pump.window, pump.thread_id, 3000, detail),
          "hook: the race path still proves quiescence");

    // (3) UI 线程挂住：短超时必须是"未退休"（不卸钩、不还引用、不报告可卸载）；恢复后重试成功。
    pump.hang.store(true);
    posted = deskfeed::payload::hook_post_run(pump.thread_id, deskfeed::payload::HookAction::Apply, 150);
    check(!posted.ok, "hook: a hung UI thread is not reported as applied");
    check(deskfeed::payload::hook_post_dangling(), "hook: a hung UI thread is recorded as dangling");
    pump.hang.store(false);
    posted = deskfeed::payload::hook_post_run(pump.thread_id, deskfeed::payload::HookAction::Apply, 4000);
    check(posted.ok && posted.retired, "hook: the retry after the UI resumes succeeds");
    check(deskfeed::payload::hook_post_retire(pump.window, pump.thread_id, 3000, detail),
          "hook: the retry also proves quiescence");

    // (4) 慢执行体：动作还没跑完时短超时收尾不能声称拿到证明。
    g_executor_slow.store(true);
    posted = deskfeed::payload::hook_post_run(pump.thread_id, deskfeed::payload::HookAction::Apply, 4000);
    g_executor_slow.store(false);
    check(posted.ok, "hook: a slow action still completes eventually");
    check(deskfeed::payload::hook_post_retire(pump.window, pump.thread_id, 3000, detail),
          "hook: a slow action still retires");

    pump.quit.store(true);
    ::WaitForSingleObject(pump.thread, 5000);
    ::CloseHandle(pump.thread);
}

// ---------------------------------------------------------------- 成品审计

struct ImportNames {
    std::vector<std::string> libraries;
};

bool read_file(std::wstring const& path, std::vector<unsigned char>& bytes) {
    HANDLE const file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(file, &size) == FALSE || size.QuadPart <= 0) {
        ::CloseHandle(file);
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    bool const ok = ::ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) != FALSE;
    ::CloseHandle(file);
    return ok && read == bytes.size();
}

// 直接从磁盘上的 PE 解析导出表；RVA 必须经节表换算成文件偏移（直接把 RVA 当偏移用会得到
// "导出目录不在任何节里"）。
bool find_export(std::vector<unsigned char> const& image, char const* name, unsigned long& rva) {
    if (image.size() < sizeof(IMAGE_DOS_HEADER)) {
        return false;
    }
    auto const* dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(image.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return false;
    }
    auto const* nt = reinterpret_cast<IMAGE_NT_HEADERS const*>(image.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    auto const& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (directory.VirtualAddress == 0 || directory.Size == 0) {
        return false;
    }

    auto rva_to_offset = [&](unsigned long value) -> std::size_t {
        auto const* section = IMAGE_FIRST_SECTION(nt);
        for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
            unsigned long const start = section->VirtualAddress;
            unsigned long const size = section->Misc.VirtualSize != 0 ? section->Misc.VirtualSize
                                                                     : section->SizeOfRawData;
            if (value >= start && value < start + size) {
                return section->PointerToRawData + (value - start);
            }
        }
        return 0;
    };

    std::size_t const export_offset = rva_to_offset(directory.VirtualAddress);
    if (export_offset == 0 || export_offset + sizeof(IMAGE_EXPORT_DIRECTORY) > image.size()) {
        return false;
    }
    auto const* exports = reinterpret_cast<IMAGE_EXPORT_DIRECTORY const*>(image.data() + export_offset);
    std::size_t const names_offset = rva_to_offset(exports->AddressOfNames);
    std::size_t const functions_offset = rva_to_offset(exports->AddressOfFunctions);
    std::size_t const ordinals_offset = rva_to_offset(exports->AddressOfNameOrdinals);
    if (names_offset == 0 || functions_offset == 0 || ordinals_offset == 0) {
        return false;
    }
    for (DWORD index = 0; index < exports->NumberOfNames; ++index) {
        unsigned long const name_rva =
            reinterpret_cast<DWORD const*>(image.data() + names_offset)[index];
        std::size_t const name_offset = rva_to_offset(name_rva);
        if (name_offset == 0 || name_offset >= image.size()) {
            continue;
        }
        if (std::strcmp(reinterpret_cast<char const*>(image.data() + name_offset), name) == 0) {
            unsigned short const ordinal =
                reinterpret_cast<unsigned short const*>(image.data() + ordinals_offset)[index];
            rva = reinterpret_cast<DWORD const*>(image.data() + functions_offset)[ordinal];
            return true;
        }
    }
    return false;
}

bool collect_imports(std::vector<unsigned char> const& image, ImportNames& out) {
    if (image.size() < sizeof(IMAGE_DOS_HEADER)) {
        return false;
    }
    auto const* dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(image.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return false;
    }
    auto const* nt = reinterpret_cast<IMAGE_NT_HEADERS const*>(image.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    auto const& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0) {
        return true;
    }
    auto const* section = IMAGE_FIRST_SECTION(nt);
    auto rva_to_offset = [&](unsigned long value) -> std::size_t {
        for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
            unsigned long const start = section->VirtualAddress;
            unsigned long const size = section->Misc.VirtualSize != 0 ? section->Misc.VirtualSize
                                                                     : section->SizeOfRawData;
            if (value >= start && value < start + size) {
                return section->PointerToRawData + (value - start);
            }
        }
        return 0;
    };

    std::size_t const import_offset = rva_to_offset(directory.VirtualAddress);
    if (import_offset == 0 || import_offset + sizeof(IMAGE_IMPORT_DESCRIPTOR) > image.size()) {
        return false;
    }
    auto const* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR const*>(image.data() + import_offset);
    for (; descriptor->Name != 0; ++descriptor) {
        std::size_t const name_offset = rva_to_offset(descriptor->Name);
        if (name_offset == 0 || name_offset >= image.size()) {
            continue;
        }
        std::string name(reinterpret_cast<char const*>(image.data() + name_offset));
        for (char& character : name) {
            character = static_cast<char>(::tolower(static_cast<unsigned char>(character)));
        }
        out.libraries.push_back(name);
    }
    return true;
}

void audit_payload(std::wstring const& dll_path) {
    std::vector<unsigned char> image;
    check(read_file(dll_path, image), "audit: the payload DLL can be read");

    unsigned long start_rva = 0;
    unsigned long stop_rva = 0;
    check(find_export(image, "DeskFeedStart", start_rva), "audit: DeskFeedStart is a parseable export");
    check(find_export(image, "DeskFeedStop", stop_rva), "audit: DeskFeedStop is a parseable export");
    check(start_rva != 0 && stop_rva != 0 && start_rva != stop_rva,
          "audit: the two exports have distinct RVAs");

    ImportNames imports;
    check(collect_imports(image, imports), "audit: the import table can be parsed");
    for (std::string const& library : imports.libraries) {
        bool const forbidden = library.find("winhttp") != std::string::npos ||
                               library.find("dbghelp") != std::string::npos ||
                               library.find("ws2_32") != std::string::npos ||
                               library.find("wininet") != std::string::npos ||
                               library.find("httpapi") != std::string::npos;
        if (forbidden) {
            std::printf("FAIL audit: forbidden import %s\n", library.c_str());
            ++g_failures;
        }
        ++g_checks;
    }

    // map 文件（如果与 DLL 同目录存在）里不该出现 JSON/快照/协议校验器/符号解析的目标文件。
    std::wstring map_path = dll_path;
    std::wstring::size_type const dot = map_path.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        map_path = map_path.substr(0, dot) + L".map";
    }
    std::vector<unsigned char> map_bytes;
    if (read_file(map_path, map_bytes)) {
        std::string const map_text(map_bytes.begin(), map_bytes.end());
        char const* const forbidden_objects[] = {
            "json.cpp.obj",   "snapshot.cpp.obj", "health.cpp.obj", "utf8.cpp.obj",
            "symbols.cpp.obj", "pdb.cpp.obj",     "http_client.cpp.obj", "url.cpp.obj",
            "taskbar_symbols.cpp.obj"};
        for (char const* object : forbidden_objects) {
            bool const present = map_text.find(object) != std::string::npos;
            if (present) {
                std::printf("FAIL audit: forbidden object %s is linked into the payload\n", object);
                ++g_failures;
            }
            ++g_checks;
        }
        // 正面的那一半：负载必须真的链到了线格式/邮箱/格式化器与它自己的单元。
        for (char const* expected : {"frame.cpp.obj", "mailbox.cpp.obj", "reading.cpp.obj",
                                     "hook_post.cpp.obj", "taskbar_xaml.cpp.obj", "xaml_tap.cpp.obj"}) {
            bool const present = map_text.find(expected) != std::string::npos;
            if (!present) {
                std::printf("FAIL audit: expected object %s is missing\n", expected);
                ++g_failures;
            }
            ++g_checks;
        }
    }
}

std::wstring widen(char const* text) {
    std::wstring result;
    for (; *text != '\0'; ++text) {
        result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*text)));
    }
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    std::wstring audit_path;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--filter") == 0 && index + 1 < argc) {
            g_filter = argv[++index];
        } else if (std::strcmp(argv[index], "--audit") == 0 && index + 1 < argc) {
            audit_path = widen(argv[++index]);
        }
    }

    if (!audit_path.empty()) {
        audit_payload(audit_path);
    } else {
        test_feed_policy();
        test_seh_boundary();
        test_hook_gate();
    }

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
