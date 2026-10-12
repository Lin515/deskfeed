// 测试专用**夹具负载** DLL：扮演 `deskfeed-payload.dll` 在注入器眼里的样子。
//
// 为什么必须有它：`do_load` / `do_stop` / `do_status` / `--watch` 现在每条路径都受 per-target 租约
// 保护，并且每一次远程调用都要按写序记账、park 到**真实**结论。只咬纯函数或单个原语的用例证明不了
// 这条链真的接上了：必须有真实的导出被真实的远程线程调用，用例才能一边读命名记录、一边看它从
// `PendingNoTid` 走到 `PendingTid` 再回到 `Idle`。
//
// 边界（重要）：本文件只被工具用例装进**用例自己 spawn 的隐藏夹具主机**。它不是生产负载，
// 不导出任何生产 ABI 之外的东西，也不会被部署；生产负载（`src/payload`）由 Native 作者维护。
//
// 行为开关一律走**环境变量**（主机进程的环境，用例 spawn 时逐个子用例设定），所以同一个 DLL 能演
// "立刻返回"、"卡在闸门上等用例放行"、"报未退休"等全部形状，而不需要为每种形状单独编一份。
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "payload/status_pod.h"

namespace {

std::uint32_t env_u32(wchar_t const* name, std::uint32_t fallback) {
    wchar_t buffer[64]{};
    DWORD const length =
        ::GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer)) {
        return fallback;
    }
    return static_cast<std::uint32_t>(::wcstoul(buffer, nullptr, 10));
}

std::wstring env_text(wchar_t const* name) {
    wchar_t buffer[256]{};
    DWORD const length =
        ::GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer)) {
        return std::wstring();
    }
    return std::wstring(buffer, length);
}

// 测试闸门：用例先在父进程里建好这个命名事件（未置位），夹具进到这里就停住，直到用例把事件置位。
// **有界等待**（30 秒）是给"用例自己崩了"兜底的，正常路径由用例决定什么时候放行。这条闸门让
// "远程调用还没回来（记录是 PendingTid）"成为一个确定性的窗口，而不是靠 sleep 抢时间。
void wait_gate(wchar_t const* name) {
    std::wstring const gate = env_text(name);
    if (gate.empty()) {
        return;
    }
    HANDLE const event = ::OpenEventW(SYNCHRONIZE, FALSE, gate.c_str());
    if (event == nullptr) {
        return;
    }
    ::WaitForSingleObject(event, 30000);
    ::CloseHandle(event);
}

}  // namespace

extern "C" __declspec(dllexport) DWORD WINAPI DeskFeedStart(void* scope) {
    (void)scope;
    wait_gate(L"DESKFEED_FIXTURE_START_GATE");
    return env_u32(L"DESKFEED_FIXTURE_START_CODE", 1);
}

extern "C" __declspec(dllexport) DWORD WINAPI DeskFeedStop(void* encoded_timeout) {
    (void)encoded_timeout;  // 冻结 ABI：超时值编码在指针本身里，夹具不解引用
    wait_gate(L"DESKFEED_FIXTURE_STOP_GATE");
    return env_u32(L"DESKFEED_FIXTURE_STOP_CODE", 1);
}

extern "C" __declspec(dllexport) DWORD WINAPI DeskFeedGetStatus(void* raw) {
    if (raw == nullptr) {
        return 0;
    }
    wait_gate(L"DESKFEED_FIXTURE_STATUS_GATE");

    auto* const pod = static_cast<deskfeed::payload::PayloadStatusPod*>(raw);
    if (pod->size != sizeof(*pod) || pod->abi != deskfeed::payload::kPayloadStatusAbi) {
        // 与真实负载同一套语义：不认就写回 abi = 0，调用方据此报"这个版本没有状态 ABI"。
        pod->abi = 0;
        return 0;
    }
    std::uint32_t const abi_override = env_u32(L"DESKFEED_FIXTURE_STATUS_ABI", 0);
    if (abi_override != 0) {
        pod->abi = abi_override;  // 让"写回一个我们不认识的 ABI"成为可构造的形状
    }

    pod->width = deskfeed::payload::kPayloadStatusWidth;
    pod->started = env_u32(L"DESKFEED_FIXTURE_STATUS_STARTED", 0);
    pod->thread_alive = env_u32(L"DESKFEED_FIXTURE_STATUS_THREAD_ALIVE", 0);
    pod->unload_safe = env_u32(L"DESKFEED_FIXTURE_STATUS_UNLOAD_SAFE", 1);
    pod->dangling = env_u32(L"DESKFEED_FIXTURE_STATUS_DANGLING", 0);
    pod->mailbox_attached = env_u32(L"DESKFEED_FIXTURE_STATUS_MAILBOX_ATTACHED", 0);
    pod->producer_alive = env_u32(L"DESKFEED_FIXTURE_STATUS_PRODUCER_ALIVE", 0);
    pod->xaml_block_present = env_u32(L"DESKFEED_FIXTURE_STATUS_BLOCK_PRESENT", 0);
    pod->producer_pid_low = env_u32(L"DESKFEED_FIXTURE_STATUS_PRODUCER_PID", 0);
    pod->producer_pid_high = 0;
    pod->frame_generation = env_u32(L"DESKFEED_FIXTURE_STATUS_GENERATION", 0);
    pod->feed_failures = 0;
    pod->producer_start_time = 0;
    pod->reserved = 0;

    std::wstring const scope = env_text(L"DESKFEED_FIXTURE_STATUS_SCOPE");
    std::size_t const count = scope.size() < deskfeed::payload::kPayloadScopeCapacity - 1
                                  ? scope.size()
                                  : deskfeed::payload::kPayloadScopeCapacity - 1;
    for (std::size_t index = 0; index < count; ++index) {
        pod->scope[index] = scope[index];
    }
    pod->scope[count] = L'\0';
    return 1;
}
