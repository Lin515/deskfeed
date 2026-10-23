#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/health.h"          // ViewKind / FailureKind（只用枚举，不拉进状态机的实现）
#include "core/snapshot.h"        // ItemState / 容量常量（协议边界只有这一处出处）
#include "display/taskbar_layout.h"  // 屏幕配置的容量上限（屏幕数量的唯一出处）

// 显示帧：主进程算出显示模型（View）之后，把它序列化成一块**有界的紧凑二进制载荷**，
// 放进共享内存交给任务栏负载 DLL。这是“主进程 → 显示邮箱”这条 seam 的唯一线格式。
//
// 设计要点（与 docs/taskbar-injection.md 的“显示邮箱”一致）：
//   * 载荷是纯通用的：只有 kind / alarm / failure / reason 与每条 item 的
//     id / label / value / state / detail。没有任何业务语义，负载 DLL 不做解释。
//   * 有界：载荷上限由协议容量边界算出（常量只在 core/snapshot.h 定义），
//     解码时逐字段做边界检查，越界即整帧拒绝，绝不无界读取共享内存。
//   * 编解码只有这一份实现，主进程与负载不可能对线格式有第二种解释。
//   * 这一层不碰 Windows 共享内存，是纯逻辑，可以单独测。
namespace deskfeed {

inline constexpr std::uint32_t kFrameMagic = 0x32464644;  // 'D','F','F','2'
// v2：在读数之前增加“任务栏屏幕配置”一节（是否显式配置 + 启用的屏幕标识表）。
inline constexpr std::uint32_t kFrameVersion = 2;

// 整体失败原因的长度上限（实现选择）。失败原因是主进程自己写的一行说明，
// 不是生产端文本，所以单独给一个上限而不是套用 detail 的。
inline constexpr std::size_t kFrameMaxReasonBytes = 1024;

// 载荷上限：64 条各自的 id/label/value/detail 加上每条几个长度字段与状态，外加失败原因。
inline constexpr std::size_t kFramePerItemOverhead = 5 * sizeof(std::uint32_t);
// 单个屏幕标识在线上格式里的字节上限。显示器接口路径是 ASCII，正常约 100 字节；给到 1024
// 是为了让手写的异常配置也能被完整带到读端，再由长度/字符合法性整帧拒绝。
inline constexpr std::size_t kMaxTaskbarScreenIdBytes = 1024;
inline constexpr std::size_t kFrameScreensOverhead = 2 * sizeof(std::uint32_t);
inline constexpr std::size_t kFrameScreensCapacity =
    kMaxTaskbarScreens * (kMaxTaskbarScreenIdBytes + sizeof(std::uint32_t)) +
    kFrameScreensOverhead;
inline constexpr std::size_t kFramePayloadCapacity =
    kMaxItems * (kMaxIdBytes + kMaxLabelBytes + kMaxValueBytes + kMaxDetailBytes +
                 kFramePerItemOverhead) +
    kFrameMaxReasonBytes + kFrameScreensCapacity + 8 * sizeof(std::uint32_t);

// 显示帧里的一条。字段与 core/health.h 的 ViewItem 一一对应。
struct FrameItem {
    std::string id;
    std::string label;
    std::string value;
    ItemState state = ItemState::Ok;
    std::string detail;
};

// 解码出来的一帧，外加邮箱自己带的元数据（代次、生产时间、是否已关闭）。
struct Frame {
    std::uint64_t generation = 0;
    std::uint64_t produced_at_ms = 0;
    bool closed = false;  // 主进程正常收尾：读到就清理任务栏组件
    ViewKind kind = ViewKind::Waiting;
    bool alarm = false;
    FailureKind failure = FailureKind::None;
    std::string reason;
    // 任务栏屏幕配置（v2）。`screens_configured=false` 表示设置文件里没有 `[taskbar] screens`，
    // 读端按“只在主屏显示”的向后兼容缺省处理；为真时 `screens` 是**启用集合**，空表表示
    // 用户明确配置为不在任何任务栏显示（这种配置不触发主屏退化）。
    bool screens_configured = false;
    std::vector<std::string> screens;
    std::vector<FrameItem> items;
};

// 把显示模型（View）映射成显示帧（Frame）：只搬字段，不做任何解释。
//
// 这是**唯一**一份 View → Frame 映射：主进程发布显示邮箱用它，离线量具（tools）算同一份
// 读数也用它是为了让“量具看到的帧”与“主进程真的发出去的帧”逐字段一致，不必各自抄一份。
// 它只依赖 core/health.h 里的 View 类型，不拉进 HealthState 的实现，也不碰 HTTP / JSON。
Frame frame_from_view(View const& view);

// 把一帧编码成载荷。超过 kFramePayloadCapacity 时返回 false（调用方应当先做容量检查；
// 正常情况下协议边界保证了不会超）。
bool frame_encode(Frame const& frame, std::vector<unsigned char>& out);

// 从载荷解码。任何长度、顺序、枚举或边界不一致都返回 false，out 不被使用。
bool frame_decode(unsigned char const* payload, std::size_t payload_bytes, Frame& out);

// 给负载 DLL 的日志用的一行文本名（不依赖 core 里的 name 函数，避免把 JSON 解析器
// 也链进 explorer 里的模块）。
char const* frame_view_kind_name(ViewKind kind);
char const* frame_failure_name(FailureKind failure);
char const* frame_item_state_name(ItemState state);

}  // namespace deskfeed
