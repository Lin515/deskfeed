#include "payload/xaml_tap.h"

#include "platform/log.h"
#include "platform/win32_util.h"

#include <windows.h>

#include <xamlom.h>

#include <winrt/base.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace deskfeed::payload {
namespace {

// 本模块作为 TAP 提供的类 id。它只属于本项目：XAML 是经本模块的 DllGetClassObject 拿这个
// 类对象的，从不经过注册表。
// {2F1B7A64-9C3E-4B0D-8E75-6A4C2D9F1B30}
constexpr GUID kTapClassId = {0x2f1b7a64, 0x9c3e, 0x4b0d, {0x8e, 0x75, 0x6a, 0x4c, 0x2d, 0x9f, 0x1b, 0x30}};

// XAML 核心在 core messaging 里按编号端口名注册自己，一个核心一个端口。问到一个没有核心拥有的
// 名字，就是搜索的终点。
constexpr unsigned kMaxConnectionIndex = 16;

// 每个**真的交出了站点**的 XAML 核心给本模块记一次加载器引用：不是每进程一次，也不是每个
// “端口应答了”一次。端口应答会让 XAML 加载本模块去创建 TAP 类对象，但如果那条连接在交出站点
// 之前就被丢掉，那份引用由 XAML 自己还。本模块的 DllCanUnloadNow 恒返回 S_FALSE，所以随站点
// 一起到来的引用只能由我们自己还。
//
// 两半都是量出来的（自测宿主自己拥有 1 / 2 / 3 个 XAML 核心，见 src/probe/README.md）：三个
// 端口应答、只有两个站点交出来时，按端口数还三次——最后一次 FreeLibrary 会在本模块自己的代码
// 还在跑的时候把它从内存里抽走。归还次数只能来自**真正释放掉的站点数**，而且与清空站点列表
// 必须在同一个临界区里完成。
using InitializeXamlDiagnosticsExFn = decltype(&InitializeXamlDiagnosticsEx);

// XAML 在 XamlDiagnostics 上也实现了这个接口，xamlom.h 没有声明它。UnregisterInstance 会关掉
// 诊断为某个上报句柄缓存的那个运行时对象——不还回去，被走查过的元素会被诊断一直持有。
constexpr GUID kXamlDiagnosticsTestHooksId = {
    0x735941a2, 0x3ee3, 0x495a, {0x8d, 0xa9, 0x97, 0x26, 0x27, 0x00, 0x30, 0x75}};

struct IXamlDiagnosticsTestHooks : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE UnregisterInstance(InstanceHandle handle) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetDispatcherQueueForObject(InstanceHandle handle,
                                                                    void** dispatcher_queue) = 0;
};

// 一次走查找到的东西。走查跨两条线程——它从工作线程发起（核心把它编组到自己的线程并等待），
// 而它指名的元素属于核心线程——所以句柄在两次调用之间保存，而不是在回调里解析。
struct Watch {
    std::vector<InstanceHandle> elements;
    std::vector<InstanceHandle> reported;
};

struct Site {
    XamlTapSite info;
    Watch watch;
};

struct State {
    std::mutex mutex;
    // 站点一律由 shared_ptr 持有，绝不按值存放：一次走查会把指向站点的指针交给调用方使用到
    // 走查结束，而走查本身跑在核心线程上（XAML 把它编组过去、调用方在等）。这期间有核心
    // 连上来会让这个 vector 扩容，按值存放时扩容会搬走每一个元素，走查就会经由失效指针写堆。
    std::vector<std::shared_ptr<Site>> sites;
    bool attempted = false;
    // 由 xaml_tap_stop 在拿走站点列表之前置位，之后永不清除：从那一刻起这条连接正在拆除，
    // 迟到的站点被拒收，它的那份引用由 XAML 从调用方那里收回，而不是本模块留下一条没人会释放
    // 的连接。
    bool stopping = false;
    // 已经进入、尚未返回的 SetSite 调用数。xaml_tap_stop 要等它归零才归还模块引用：还在跑的
    // SetSite 就是本模块自己的代码，把最后一份引用还回去等于在它脚下把模块抽掉。
    unsigned set_site_depth = 0;
    std::condition_variable sites_changed;
    // 应答过诊断的端口数。只作为与"真的交出站点的核心"对照的交叉检查：归还次数取自站点
    // （见 xaml_tap_stop），两者的差值是值得记一笔的泄漏，而不是多还几次 FreeLibrary 的理由。
    unsigned references_taken = 0;
};

State g_state;

// 手写的最小 COM 对象：负载是普通 Win32 DLL，XAML 向我们要的两个对象这样写比走 C++/WinRT 的
// implements（那是给 WinRT 类用的）更省。
template <typename Interface>
class ComObject : public Interface {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interface_id, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (::IsEqualIID(interface_id, __uuidof(Interface)) || ::IsEqualIID(interface_id, IID_IUnknown)) {
            *object = static_cast<Interface*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(::InterlockedIncrement(&references_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        LONG remaining = ::InterlockedDecrement(&references_);
        if (remaining == 0) {
            delete this;
        }
        return static_cast<ULONG>(remaining);
    }

protected:
    virtual ~ComObject() = default;

private:
    LONG references_ = 1;
};

// COM 入口的故障记录：异常码 + 相对出错模块的偏移。这个函数里不得有需要析构的局部对象。
int com_fault_filter(EXCEPTION_POINTERS* info) {
    unsigned long code = 0;
    void* address = nullptr;
    if (info != nullptr && info->ExceptionRecord != nullptr) {
        code = static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode);
        address = info->ExceptionRecord->ExceptionAddress;
    }
    HMODULE module = nullptr;
    wchar_t name[MAX_PATH]{};
    unsigned long long offset = 0;
    if (address != nullptr &&
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(address), &module) != FALSE &&
        ::GetModuleFileNameW(module, name, static_cast<DWORD>(std::size(name))) != 0) {
        offset = static_cast<unsigned long long>(reinterpret_cast<unsigned char*>(address) -
                                                 reinterpret_cast<unsigned char*>(module));
    }
    log_format(L"tap: hardware fault 0x%08lX at %s+0x%llX", code,
               name[0] != L'\0' ? name : L"<unknown>", offset);
    return EXCEPTION_EXECUTE_HANDLER;
}

// 站点对象。XAML 在 SetSite 返回之后立刻释放它，所以模块想留下的东西必须在这里拿走。
class TapSite : public ComObject<IObjectWithSite> {
public:
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) override;
    HRESULT STDMETHODCALLTYPE GetSite(REFIID interface_id, void** object) override;
};

// SetSite 的 C++ 部分：有锁、有 shared_ptr、可能抛 bad_alloc，所以它只从 SEH 外壳里被调用。
HRESULT set_site_cpp(IUnknown* site) {
    if (site == nullptr) {
        return S_OK;
    }

    winrt::com_ptr<IXamlDiagnostics> diagnostics;
    HRESULT query = site->QueryInterface(__uuidof(IXamlDiagnostics), diagnostics.put_void());
    if (FAILED(query)) {
        log_format(L"  tap: the site is not IXamlDiagnostics: %s", hresult_text(query).c_str());
        return query;
    }

    size_t count = 0;
    bool refused = false;
    {
        // 挂上与"这条连接还要不要"是同一个锁里的一个决定：要么这次算一次挂上、由本模块持有
        // XAML 为它取的那份引用；要么连接正在拆除，那份引用随调用方一起还给 XAML——XAML 在
        // 这次调用返回时就会释放它为这个端口创建的类对象，它借出去的那一份引用就花在那里。
        std::lock_guard<std::mutex> guard(g_state.mutex);
        if (g_state.stopping) {
            refused = true;
        } else {
            auto entry = std::make_shared<Site>();
            entry->info.thread_id = ::GetCurrentThreadId();
            entry->info.diagnostics = diagnostics.detach();
            // 从这一刻到本次调用结束都算在途：站点已经进列表，但本函数还在跑本模块的代码，
            // 正在归还引用的 stop 必须让它跑完。
            ++g_state.set_site_depth;
            g_state.sites.push_back(entry);
            count = g_state.sites.size();
        }
    }
    if (refused) {
        log_format(L"  tap: a site arrived while the diagnostics were closing; refused on thread %lu",
                   ::GetCurrentThreadId());
        return S_OK;
    }

    log_format(L"  tap: core %zu attached on thread %lu", count, ::GetCurrentThreadId());
    {
        std::lock_guard<std::mutex> guard(g_state.mutex);
        --g_state.set_site_depth;
        g_state.sites_changed.notify_all();
    }
    return S_OK;
}

// SEH 外壳：只允许引用参数，函数里不得有需要展开的局部对象。
bool set_site_seh(IUnknown* site, HRESULT& out) {
    __try {
        out = set_site_cpp(site);
        return true;
    } __except (com_fault_filter(GetExceptionInformation())) {
        out = E_FAIL;
        return false;
    }
}

HRESULT STDMETHODCALLTYPE TapSite::SetSite(IUnknown* site) {
    HRESULT result = E_FAIL;
    try {
        if (!set_site_seh(site, result)) {
            return E_FAIL;
        }
    } catch (...) {
        log_line(L"tap: SetSite threw; the site is dropped");
        return E_FAIL;
    }
    return result;
}

HRESULT STDMETHODCALLTYPE TapSite::GetSite(REFIID, void** object) {
    if (object != nullptr) {
        *object = nullptr;
    }
    return E_NOTIMPL;
}

class TapFactory : public ComObject<IClassFactory> {
public:
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID interface_id,
                                             void** object) override;
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
};

HRESULT STDMETHODCALLTYPE TapFactory::CreateInstance(IUnknown* outer, REFIID interface_id,
                                                     void** object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;
    if (outer != nullptr) {
        return CLASS_E_NOAGGREGATION;
    }

    auto* site = new (std::nothrow) TapSite();
    if (site == nullptr) {
        return E_OUTOFMEMORY;
    }
    HRESULT result = site->QueryInterface(interface_id, object);
    site->Release();
    return result;
}

// AdviseVisualTreeChange 把树上报给这个回调。它跑在核心线程上，而且**绝不能失败**：走查会在
// 第一个返回错误的回调处停下，剩下的树就再也报不上来了。
//
// VisualElement 按值收到、字符串归回调所有，所以在这里释放；只记住名字对上的那个元素，句柄还
// 不解析。
class TreeWalk : public IVisualTreeServiceCallback2 {
public:
    explicit TreeWalk(std::wstring name) : name_(std::move(name)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interface_id, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (::IsEqualIID(interface_id, IID_IUnknown) ||
            ::IsEqualIID(interface_id, __uuidof(IVisualTreeServiceCallback)) ||
            ::IsEqualIID(interface_id, __uuidof(IVisualTreeServiceCallback2))) {
            *object = static_cast<IVisualTreeServiceCallback2*>(this);
            AddRef();
            return S_OK;
        }
        // 刻意不做 IVisualTreeServiceCallback3：它还会收到每棵树的 XamlRoot，本模块用不到。
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(::InterlockedIncrement(&references_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        LONG remaining = ::InterlockedDecrement(&references_);
        if (remaining == 0) {
            delete this;
        }
        return static_cast<ULONG>(remaining);
    }

    // C++ 部分。BSTR 归回调所有，必须释放。
    void report(VisualElement element) {
        ++seen_;

        std::wstring name = element.Name != nullptr ? element.Name : L"";
        std::wstring type = element.Type != nullptr ? element.Type : L"";

        if (watch_ != nullptr) {
            watch_->reported.push_back(element.Handle);
            if (name == name_) {
                watch_->elements.push_back(element.Handle);
                log_format(L"  found %s as %s#%s", name_.c_str(), type.c_str(), name.c_str());
            }
        }
        // 这一行先攒着，等整棵树走完再决定写不写：只有**没找到**目标时它才有诊断价值。
        // 直接在这里写的话，一次成功的走查也会把整棵树的清单倒进日志（任务栏那棵树有几百行）。
        if (types_.size() < kTypesLogged) {
            types_.push_back(type + L"#" + name);
        }

        if (element.Type != nullptr) {
            ::SysFreeString(element.Type);
        }
        if (element.Name != nullptr) {
            ::SysFreeString(element.Name);
        }
        if (element.SrcInfo.FileName != nullptr) {
            ::SysFreeString(element.SrcInfo.FileName);
        }
        if (element.SrcInfo.Hash != nullptr) {
            ::SysFreeString(element.SrcInfo.Hash);
        }
    }

    // IVisualTreeServiceCallback
    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation, VisualElement element,
                                                 VisualMutationType) override {
        HRESULT result = S_OK;
        try {
            if (!report_seh(element, result)) {
                return S_OK;  // 故障已经记在 filter 里；回调必须回 S_OK，否则走查中断。
            }
        } catch (...) {
            log_line(L"tap: OnVisualTreeChange threw; the walk keeps going");
        }
        return S_OK;
    }

    // IVisualTreeServiceCallback2
    HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle, VisualElementState,
                                                    LPCWSTR) noexcept override {
        return S_OK;
    }

    void set_watch(Watch* watch) { watch_ = watch; }
    unsigned seen() const { return seen_; }

    // 没找到目标时把攒下来的树清单写出来（找元素只能靠它）。
    void dump_types() {
        for (std::wstring const& line : types_) {
            log_format(L"  tree: %s", line.c_str());
        }
        types_.clear();
    }

private:
    // SEH 外壳：VisualElement 没有析构对象，可以按值传进来。
    bool report_seh(VisualElement element, HRESULT& out) {
        __try {
            report(element);
        } __except (com_fault_filter(GetExceptionInformation())) {
            out = E_FAIL;
            return false;
        }
        return true;
    }

    // 有界：走查 shell 的树不能把日志写满。
    static constexpr unsigned kTypesLogged = 40;

    LONG references_ = 1;
    std::wstring name_;
    Watch* watch_ = nullptr;
    unsigned seen_ = 0;
    std::vector<std::wstring> types_;
};

HRESULT get_class_object_cpp(REFCLSID class_id, REFIID interface_id, void*& out) {
    if (!::IsEqualGUID(class_id, kTapClassId)) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    auto* factory = new (std::nothrow) TapFactory();
    if (factory == nullptr) {
        return E_OUTOFMEMORY;
    }
    HRESULT result = factory->QueryInterface(interface_id, &out);
    factory->Release();
    return result;
}

bool get_class_object_seh(REFCLSID class_id, REFIID interface_id, void*& out, HRESULT& result) {
    __try {
        result = get_class_object_cpp(class_id, interface_id, out);
        return true;
    } __except (com_fault_filter(GetExceptionInformation())) {
        result = E_FAIL;
        return false;
    }
}

}  // namespace

// 每个 COM 入口都有 C++ 与 SEH 两道边界：这里被 XAML 调用，异常穿出去就是带走宿主进程。
HRESULT tap_get_class_object(REFCLSID class_id, REFIID interface_id, void** object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;
    void* created = nullptr;
    HRESULT result = E_FAIL;
    try {
        if (!get_class_object_seh(class_id, interface_id, created, result)) {
            return E_FAIL;
        }
    } catch (...) {
        log_line(L"tap: DllGetClassObject threw");
        return E_FAIL;
    }
    *object = created;
    return result;
}

bool xaml_tap_start(std::wstring& error) {
    {
        std::lock_guard<std::mutex> guard(g_state.mutex);
        if (g_state.stopping) {
            // 这个进程里的连接已经拆掉了（一次加载只能走查一次），本模块无法再应答端口。
            // 想要新连接的调用方必须重新加载本模块。
            log_line(L"the diagnostics connection was already closed; this module cannot connect again");
            error = L"the XAML diagnostics connection of this module is already closed";
            return false;
        }
        if (g_state.attempted) {
            return !g_state.sites.empty();
        }
        g_state.attempted = true;
    }

    HMODULE xaml = ::LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (xaml == nullptr) {
        error = L"Windows.UI.Xaml.dll cannot be loaded: " + last_error_text();
        return false;
    }

    auto initialize = reinterpret_cast<InitializeXamlDiagnosticsExFn>(
        ::GetProcAddress(xaml, "InitializeXamlDiagnosticsEx"));
    if (initialize == nullptr) {
        error = L"Windows.UI.Xaml.dll does not export InitializeXamlDiagnosticsEx";
        return false;
    }

    std::wstring self = module_self_path();
    if (self.empty()) {
        error = L"cannot determine the path of the payload module";
        return false;
    }

    log_format(L"connecting to the XAML diagnostics ports of this process (tap %s)", self.c_str());

    unsigned accepted = 0;
    for (unsigned index = 1; index <= kMaxConnectionIndex; ++index) {
        wchar_t connection[64];
        ::swprintf(connection, 64, L"VisualDiagConnection%u", index);

        HRESULT result = initialize(connection, ::GetCurrentProcessId(), L"", self.c_str(),
                                    kTapClassId, nullptr);
        log_format(L"  %s -> %s", connection, hresult_text(result).c_str());

        if (result == E_NOTFOUND) {
            continue;
        }
        if (FAILED(result)) {
            error = std::wstring(L"InitializeXamlDiagnosticsEx(") + connection + L") -> " +
                    hresult_text(result);
            break;
        }
        ++accepted;
    }

    if (accepted == 0) {
        if (error.empty()) {
            error = L"no XAML core in this process answered the diagnostics port";
        }
        return false;
    }

    {
        // 记在取引用的地方：这样即使某个端口始终没交出站点，stop 也能按实际站点数归还。
        std::lock_guard<std::mutex> guard(g_state.mutex);
        g_state.references_taken += accepted;
    }
    log_format(L"the diagnostics answered on %u port(s); one module reference per site they hand over",
               accepted);
    return true;
}

std::vector<XamlTapSite> xaml_tap_sites() {
    std::lock_guard<std::mutex> guard(g_state.mutex);
    std::vector<XamlTapSite> result;
    result.reserve(g_state.sites.size());
    for (std::shared_ptr<Site> const& site : g_state.sites) {
        result.push_back(site->info);
    }
    return result;
}

size_t xaml_tap_wait_for_sites(size_t deadline_ms) {
    std::unique_lock<std::mutex> guard(g_state.mutex);
    // 判据是"站点数追平本轮应答的端口数"：references_taken 是唯一已知的"这一轮该来几个"的
    // 线索。它只用来决定**等多久**，不参与归还——补引用那条老路已经在第三次 FreeLibrary 上
    // 真崩过，这里一行都不碰。
    g_state.sites_changed.wait_for(
        guard, std::chrono::milliseconds(deadline_ms),
        [] { return g_state.sites.size() >= static_cast<size_t>(g_state.references_taken); });
    return g_state.sites.size();
}

namespace {

// 某个站点的诊断对象，加上属于它的走查状态。
//
// 除了两个裸指针，shared_ptr 也一并交出去：调用方在用它们期间一直持有它，所以即使期间有别的核心
// 连上来（vector 扩容）或有 stop 把列表拿走，站点对象也还活着。
bool site_object(size_t index, IXamlDiagnostics** diagnostics, std::shared_ptr<Site>* keep_alive,
                 Watch** watch, std::wstring& error) {
    std::lock_guard<std::mutex> guard(g_state.mutex);
    if (index >= g_state.sites.size()) {
        error = L"no such diagnostics site: " + std::to_wstring(index);
        return false;
    }
    std::shared_ptr<Site> site = g_state.sites[index];
    *keep_alive = site;
    *diagnostics = static_cast<IXamlDiagnostics*>(site->info.diagnostics);
    *watch = &site->watch;
    return true;
}

}  // namespace

bool xaml_tap_watch(size_t index, std::wstring const& element_name,
                    std::vector<std::uint64_t>& matches, std::wstring& error) {
    matches.clear();
    std::shared_ptr<Site> site;
    IXamlDiagnostics* diagnostics = nullptr;
    Watch* watch = nullptr;
    if (!site_object(index, &diagnostics, &site, &watch, error)) {
        return false;
    }

    winrt::com_ptr<IVisualTreeService> service;
    HRESULT query = diagnostics->QueryInterface(__uuidof(IVisualTreeService), service.put_void());
    if (FAILED(query)) {
        error = L"IXamlDiagnostics is not an IVisualTreeService: " + hresult_text(query);
        return false;
    }

    auto* walk = new (std::nothrow) TreeWalk(element_name);
    if (walk == nullptr) {
        error = L"cannot allocate the tree walk";
        return false;
    }

    // 上一次走查的句柄现在都失效了，即将跑的这次会记自己的。
    watch->elements.clear();
    watch->reported.clear();
    walk->set_watch(watch);

    // AdviseVisualTreeChange 会走遍该核心的每一个调度队列，所以它返回时回调已经见过现有的整棵树。
    HRESULT advised = service->AdviseVisualTreeChange(walk);
    HRESULT unadvised = service->UnadviseVisualTreeChange(walk);
    unsigned seen = walk->seen();
    walk->set_watch(nullptr);
    if (watch->elements.empty()) {
        // 树清单只在没找到目标时写：它是"任务栏的树长什么样"的证据，成功时不值得占日志。
        walk->dump_types();
    }
    walk->Release();

    if (FAILED(advised)) {
        error = L"AdviseVisualTreeChange -> " + hresult_text(advised);
        return false;
    }
    if (FAILED(unadvised)) {
        error = L"UnadviseVisualTreeChange -> " + hresult_text(unadvised);
        return false;
    }

    log_format(L"  walk reported %u element(s), %zu matching %s", seen, watch->elements.size(),
               element_name.c_str());
    if (watch->elements.empty()) {
        error = element_name + L" is not in the tree of this core";
        return false;
    }
    for (InstanceHandle const handle : watch->elements) {
        matches.push_back(static_cast<std::uint64_t>(handle));
    }
    return true;
}

bool xaml_tap_resolve(size_t index, std::uint64_t handle,
                      winrt::Windows::Foundation::IInspectable& element, std::wstring& error) {
    std::shared_ptr<Site> site;
    IXamlDiagnostics* diagnostics = nullptr;
    Watch* watch = nullptr;
    if (!site_object(index, &diagnostics, &site, &watch, error)) {
        return false;
    }
    if (handle == 0) {
        error = L"no element was watched yet";
        return false;
    }

    HRESULT resolved = diagnostics->GetIInspectableFromHandle(
        static_cast<InstanceHandle>(handle),
        reinterpret_cast<::IInspectable**>(winrt::put_abi(element)));
    if (FAILED(resolved) || element == nullptr) {
        error = L"GetIInspectableFromHandle -> " + hresult_text(resolved);
        return false;
    }
    return true;
}

void xaml_tap_release_walk(size_t index) {
    std::shared_ptr<Site> site;
    IXamlDiagnostics* diagnostics = nullptr;
    Watch* watch = nullptr;
    std::wstring ignored;
    if (!site_object(index, &diagnostics, &site, &watch, ignored)) {
        return;
    }
    if (watch->reported.empty()) {
        return;
    }

    // 每一个上报过的句柄都在诊断里持有一份引用，所以在意的那个元素解析到我们自己手上之后，
    // 这些句柄要还回去。
    winrt::com_ptr<IXamlDiagnosticsTestHooks> hooks;
    if (SUCCEEDED(diagnostics->QueryInterface(kXamlDiagnosticsTestHooksId, hooks.put_void()))) {
        for (InstanceHandle handle : watch->reported) {
            hooks->UnregisterInstance(handle);
        }
    } else {
        log_line(L"IXamlDiagnosticsTestHooks is unavailable; the walked handles stay registered");
    }
    log_format(L"released %zu handle(s) from the walk", watch->reported.size());

    watch->elements.clear();
    watch->reported.clear();
}

// 把这条连接取走的一切都还回去，只有这一处、且只还一次。
//
// 归还的模块引用数是**真的交出站点的核心数**，不是应答的端口数：一个端口可以应答之后永远不交
// 站点，因为 XAML 是在它为那个端口创建 TAP 类对象时取的引用，连接被丢掉时它自己会还。这一条是
// 量出来的、不是推理出来的：三个端口应答、只有两个站点挂上时，还三次会在最后一次 FreeLibrary
// 上把本模块抽走，进程死在这个函数里，宣告之后一行都没有。两个站点，两次。
//
// 所以站点数永远不会偏大：一个站点就是一条活的连接、正好持一份引用。始终没到来的站点最多让
// 模块多留一份引用（模块保持加载，注入器会重试并记日志）。端口数只留着发现这种情况。
//
// 迟到的站点不再无声丢失：这个函数在同一个锁里把"连接正在关闭"标上，之后到来的 SetSite 被
// 拒收，它借出去的那份引用由 XAML 从调用方那里收回（见 TapSite::SetSite）。
//
// 两个数在同一个临界区里花掉，所以第二次 stop 是安全的：两个数都是零，什么也不做。
//
// 返回值就是这条路径**唯一现成的真实证据**：SetSite 排空到零。超时那一刻不宣称退休（返回 false）
// 并保持模块映射；引用归还策略本身保持不变，不在这里发明新的门。
bool xaml_tap_stop(unsigned timeout_ms, std::wstring& detail) {
    std::vector<std::shared_ptr<Site>> sites;
    unsigned ports = 0;
    bool drained = true;
    {
        // 这里必须是 unique_lock：下面的等待要用它。
        std::unique_lock<std::mutex> guard(g_state.mutex);
        g_state.stopping = true;
        sites.swap(g_state.sites);
        ports = g_state.references_taken;
        g_state.references_taken = 0;

        // 已经越过标志位的 SetSite 此刻正在跑本模块的代码（它在离开锁之后还要写日志）。在它
        // 返回之前归还引用可能把模块从它脚下抽走，所以要等它——但有界：一条永远结束不了的连接
        // 不能把拆除拖死。模块多留一份可以恢复，崩了不行。
        unsigned const drain_ms = timeout_ms == 0 ? 1u : timeout_ms;
        drained = g_state.sites_changed.wait_for(guard, std::chrono::milliseconds(drain_ms),
                                                 [] { return g_state.set_site_depth == 0; });
        if (!drained) {
            detail = L"还有 SetSite 调用在期限内没有返回（在途 " +
                     std::to_wstring(g_state.set_site_depth) + L"）";
            log_format(L"a site attach is still inside SetSite after %u ms (%u in flight); the "
                       L"module must stay loaded",
                       drain_ms, static_cast<unsigned>(g_state.set_site_depth));
        }
    }

    // 先关连接：每个站点各自持有一份 IXamlDiagnostics，核心在最后一份释放时才关掉端口，那一刻
    // XAML 也会把它为"应答过但没交出站点"的端口留的那份引用还掉。
    for (std::shared_ptr<Site> const& site : sites) {
        if (site->info.diagnostics != nullptr) {
            static_cast<IXamlDiagnostics*>(site->info.diagnostics)->Release();
        }
    }
    if (!sites.empty()) {
        log_format(L"released %zu XAML diagnostics site(s)", sites.size());
    }

    // 所有文字都写在第一次 FreeLibrary 之前：下面这些调用里有一个会把本模块从内存里抽走，
    // 那一行之后这个函数一行都跑不了。
    unsigned const references = static_cast<unsigned>(sites.size());
    if (references != ports) {
        log_format(L"only %zu of %u answered port(s) attached a site; giving back %u module reference(s)",
                   sites.size(), ports, references);
    } else if (references == 1) {
        log_line(L"giving back the module reference the diagnostics took");
    } else if (references > 1) {
        log_format(L"giving back the %u module references the diagnostics took", references);
    }

    for (unsigned index = 0; index < references; ++index) {
        ::FreeLibrary(module_self_handle());
    }

    // 站点最后才销毁，而且在归还引用之后：上面释放最后一份 IXamlDiagnostics 不能是本模块
    // 失去映射之前做的最后一件事，而在这里清 shared_ptr 会跑到本模块里的删除器。
    sites.clear();
    return drained;
}

}  // namespace deskfeed::payload
