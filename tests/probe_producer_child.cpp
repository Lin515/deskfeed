// 测试专用**辅助显示帧生产者**进程：给探针的 `--frame-script ownerdeath` 提供一个真实的
// "邮箱属主是另一条进程"的形状。
//
// 为什么必须是**另一个进程**：探针自己建 producer 就成了"自己发一帧冒充 owner 死亡"，而"让宿主
// 自己去死"更是连树一起没了，什么都证明不了。负载清块靠的是它自己那条**真实进程句柄已触发**，
// 所以这条路径只有在邮箱属主真的是一条独立进程、并且真的结束时才成立。
//
// 用法（由 `deskfeed-probe.exe` 按"与本可执行文件同目录"拉起）：
//   deskfeed-probe-producer-child.exe --scope <name> --value <text> [--parent-claim] [--hold-ms <n>]
// 行为：先按 probe/scope_claim.h 拿到这个 scope 的实例仲裁（与主进程同一个名字），再建隔离 scope
// 的显示邮箱、发布**一帧**（内容 = 一个通用条目），然后驻留到被父进程结束（`--hold-ms` 只是
// "父进程崩了"时的有界兜底，默认 60 秒）；到点自行正常退出。
//
// `--parent-claim`：父进程已经持有同一个仲裁名字贯穿整段运行（父子模式），子进程不再自己去抢
// ——抢就会撞上父进程而自拒；scope 白名单与"邮箱已存在就拒绝"仍然照做。
//
// 退出码：0 = 驻留结束正常退出；2 = 参数无效；3 = 建不了邮箱；4 = 发不出帧；5 = 抢不到实例仲裁。
// 本文件只被自测工具拉起，不部署、不安装、不进任何交付二进制。
#include "display/mailbox.h"
#include "platform/paths.h"
#include "probe/scope_claim.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace {

std::string narrow(std::wstring const& text) {
    std::string out;
    out.reserve(text.size());
    for (wchar_t const character : text) {
        out.push_back(static_cast<char>(character & 0x7f));
    }
    return out;
}

long long wall_clock_ms() {
    FILETIME stamp{};
    ::GetSystemTimeAsFileTime(&stamp);
    ULARGE_INTEGER value{};
    value.LowPart = stamp.dwLowDateTime;
    value.HighPart = stamp.dwHighDateTime;
    return static_cast<long long>(value.QuadPart / 10000ull) - 11644473600000LL;
}

// 与探针的 `kDefaultValue` 同样的通用条目形状：没有任何业务语义。
deskfeed::Frame owner_frame(std::string const& value) {
    deskfeed::Frame frame;
    frame.kind = deskfeed::ViewKind::Content;
    deskfeed::FrameItem item;
    item.id = "aux";
    item.label = "AUX";
    item.value = value;
    item.state = deskfeed::ItemState::Ok;
    item.detail = "aux producer detail";
    frame.items.push_back(std::move(item));
    return frame;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring scope;
    std::string value;
    unsigned hold_ms = 60000;
    bool parent_claim = false;
    for (int index = 1; index < argc; ++index) {
        std::wstring const argument = argv[index];
        auto next = [&](std::wstring& out) {
            if (index + 1 >= argc) {
                return false;
            }
            out = argv[++index];
            return true;
        };
        if (argument == L"--scope") {
            if (!next(scope)) {
                return 2;
            }
        } else if (argument == L"--value") {
            std::wstring text;
            if (!next(text)) {
                return 2;
            }
            value = narrow(text);
        } else if (argument == L"--parent-claim") {
            parent_claim = true;
        } else if (argument == L"--hold-ms") {
            std::wstring text;
            if (!next(text)) {
                return 2;
            }
            hold_ms = static_cast<unsigned>(::wcstoul(text.c_str(), nullptr, 10));
        } else {
            std::fprintf(stderr, "producer-child: unknown argument\n");
            return 2;
        }
    }
    // scope 会被带进内核对象名与共享内存名：非法/空一律拒绝，绝不落到生产命名空间。
    std::wstring scope_error;
    if (scope.empty() || !deskfeed::scope_validate(scope, scope_error)) {
        std::fprintf(stderr, "producer-child: invalid --scope\n");
        return 2;
    }
    if (value.empty()) {
        std::fprintf(stderr, "producer-child: missing --value\n");
        return 2;
    }

    deskfeed::DisplayFrameProducer producer;
    std::string error;
    // 实例仲裁先于任何 producer：抢不到说明这个 scope 已被主进程或另一个探针占着，此时 create
    // 会把别人的帧头 memset 掉，所以只能拒绝。
    deskfeed::probe::ProbeScopeClaim claim;
    bool const claimed = parent_claim ? claim.acquire_with_parent_claim(scope, error)
                                      : claim.acquire(scope, error);
    if (!claimed) {
        std::fprintf(stderr, "producer-child: scope is not claimable (%s)\n", error.c_str());
        return 5;
    }
    if (!producer.create(scope, nullptr, error)) {
        std::fprintf(stderr, "producer-child: create failed (%s)\n", error.c_str());
        return 3;
    }
    if (!producer.publish(owner_frame(value), wall_clock_ms())) {
        std::fprintf(stderr, "producer-child: publish failed\n");
        return 4;
    }
    std::printf("producer-child: published scope=%s value=%s pid=%lu\n", narrow(scope).c_str(),
                value.c_str(), ::GetCurrentProcessId());
    std::fflush(stdout);

    // 有界驻留：父进程正常时会提前结束我们；父进程崩了这里也不会留下孤儿进程。
    ::Sleep(hold_ms);
    producer.close();
    return 0;
}
