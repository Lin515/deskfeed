// 测试专用 DLL：给"远程线程超时之后仍在写调用方缓冲"这条路径提供真实的延迟写者。
//
// 为什么必须是**另一个模块**里的导出：`remote_fetch_pod` / `call_remote_export` 在目标进程里跑的是
// 远程线程，只有真的有一个"先睡、后写"的函数，才能确定性地制造"调用已超时、写入还没发生"的窗口。
// 本文件只被测试目标使用，不进入负载、不进入任何交付二进制。
#include <windows.h>

#include <cstring>

namespace {

unsigned char const kLateWriteMagic[8] = {0x44, 0x46, 0x4C, 0x41, 0x54, 0x45, 0x01, 0x00};

}  // namespace

// 睡够 `kDelayMs` 之后，把 144 字节（含 magic）写进调用方给的缓冲，返回 1。
// 参数与返回值形状与 `DWORD WINAPI f(void*)` 的远程调用 ABI 一致。
extern "C" __declspec(dllexport) DWORD WINAPI DeskFeedTestDelayedWrite(void* buffer) {
    constexpr DWORD kDelayMs = 800;
    if (buffer == nullptr) {
        return 0;
    }
    ::Sleep(kDelayMs);

    unsigned char block[144]{};
    for (int index = 0; index < 144; ++index) {
        block[index] = static_cast<unsigned char>(index ^ 0x5A);
    }
    std::memcpy(block, kLateWriteMagic, sizeof(kLateWriteMagic));
    std::memcpy(buffer, block, sizeof(block));
    return 1;
}
