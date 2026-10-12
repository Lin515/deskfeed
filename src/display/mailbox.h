#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "display/frame.h"
#include "platform/user_scope.h"

// 显示邮箱：主进程（唯一生产者）把“最新完整显示帧”写进一块命名共享内存，并用一个命名
// 通知事件叫醒任务栏负载 DLL；DLL 只读最新一帧，不做外部 HTTP、不解释业务语义、
// 不自己判断健康或到期。
//
// 语义与硬约束（见 docs/taskbar-injection.md 的“显示邮箱”）：
//   * **只保留最新一帧，不排队积压**：载荷区只有一份，新帧覆盖旧帧；DLL 重新加载时直接
//     读当前最新帧，不需要历史。
//   * **提交是完整的**：写入时用一个跨进程的序号（seqlock）把整次写入标成“正在写”，
//     帧体与帧头写完之后才把序号置回稳定，最后才 SetEvent。读端比较前后两次序号，
//     不一致就重试；再叠加长度/版本/条目数/校验和检查，绝不无界读取共享内存。
//   * **生产者身份可验证**：帧头带生产者的 PID 与**进程创建时间**。读端打开该 PID 并
//     比对创建时间与自己的活动句柄，PID 被复用（旧主进程已死）时拒绝使用这块内存。
//   * **可唤醒的收尾**：主进程正常退出时置 closed 标志并唤醒读端；异常退出（崩溃）时
//     读端靠“生产者进程句柄已触发”自己醒来清理，不留下旧读数。
//   * 命名对象都带当前用户 SID 短哈希并只授权给当前用户，与 Phase 1 的控制通道一致。
namespace deskfeed {

// 这个 scope 对应的命名对象名。生产者、消费者与验收 harness 都从这里取，
// 免得同一条命名规则在两处各写一遍。
std::wstring display_frame_map_name(std::wstring const& scope);
std::wstring display_frame_ready_name(std::wstring const& scope);

// 共享内存里的帧头。只用定长整型；同一架构的同一份代码写、读，布局不会有两套解释。
struct FrameSharedHeader {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t header_bytes;
    std::uint32_t capacity;          // 整个映射的字节数
    std::uint32_t payload_capacity;  // 载荷区可用字节数
    std::uint32_t sequence;          // 偶数 = 稳定；奇数 = 正在写
    std::uint32_t payload_bytes;     // 本次载荷长度（只在序号稳定时有效）
    std::uint32_t item_count;
    std::uint32_t kind;              // ViewKind 的稳定编号
    std::uint32_t flags;             // FrameFlag
    std::uint32_t alarm;
    std::uint32_t failure;           // FailureKind 的稳定编号
    std::uint32_t checksum;          // 载荷的 FNV-1a 32 位
    std::uint32_t reserved;
    std::uint64_t generation;
    std::uint64_t produced_at_ms;
    std::uint64_t producer_pid;
    std::uint64_t producer_start_time;
    std::uint64_t reserved2;
};

inline constexpr std::uint32_t kFrameFlagNone = 0;
inline constexpr std::uint32_t kFrameFlagClosed = 1;  // 生产者正常收尾

// 整个映射的大小：帧头 + 载荷上限。
inline constexpr std::size_t kFrameSharedBytes =
    sizeof(FrameSharedHeader) + kFramePayloadCapacity;

static_assert(sizeof(FrameSharedHeader) % 8 == 0, "帧头必须保持 8 字节对齐");

// 生产者：主进程持有。create 失败时邮箱不可用，主进程照常运行（只是没有任务栏显示面）。
class DisplayFrameProducer {
public:
    DisplayFrameProducer() = default;
    ~DisplayFrameProducer();
    DisplayFrameProducer(DisplayFrameProducer const&) = delete;
    DisplayFrameProducer& operator=(DisplayFrameProducer const&) = delete;

    // 建立共享内存与通知事件（都不存在时创建；已存在则打开）。security 可为空。
    bool create(std::wstring const& scope, UserSecurity const* security, std::string& error);

    // 关掉自己的句柄。共享内存与事件会随最后一个句柄关闭而消失。
    void close();

    bool created() const { return view_ != nullptr; }

    // 发布一帧。载荷装不下或邮箱没建好时返回 false（调用方只记日志，不影响取数）。
    bool publish(Frame const& frame, long long now_ms);

    // 正常收尾：保留最后一帧内容，置 closed 标志并唤醒读端。
    bool publish_closed(long long now_ms);

private:
    bool write_frame(Frame const& frame, std::uint32_t extra_flags, long long now_ms);

    HANDLE mapping_ = nullptr;
    HANDLE ready_ = nullptr;
    FrameSharedHeader* view_ = nullptr;
    std::size_t capacity_ = 0;
    std::uint64_t generation_ = 0;
    std::uint64_t start_time_ = 0;
    std::wstring map_name_;
    std::wstring ready_name_;
};

// 消费者：任务栏负载 DLL 持有。只读。
class DisplayFrameConsumer {
public:
    DisplayFrameConsumer() = default;
    ~DisplayFrameConsumer();
    DisplayFrameConsumer(DisplayFrameConsumer const&) = delete;
    DisplayFrameConsumer& operator=(DisplayFrameConsumer const&) = delete;

    // 打开共享内存与事件，并校验生产者身份（PID + 进程创建时间）。失败时 error 说明原因；
    // 生产者已经不在了也算失败（调用方据此立刻清理，不留旧读数）。
    bool attach(std::wstring const& scope, std::string& error);

    void detach();

    bool attached() const { return view_ != nullptr; }

    // 等它和 ready 事件一起用：主进程异常退出时它会触发。
    HANDLE ready_event() const { return ready_; }
    HANDLE producer_process() const { return producer_; }

    // 生产者当前还在不在（句柄已触发 = 已退出）。
    bool producer_running() const;

    // 读最新一帧。读到合法且完整的一帧返回 true，out 被填好。
    //
    // **每一次读都重新校验整份帧头**，不是只在 attach 时校验一次：同一个 section 在 attach 之后
    // 仍可能被改写（旧生产者没退干净、或者同一个名字被新生产者重新建起来）。校验内容包括
    // 不可变字段（magic/version/header_bytes/capacity/payload_capacity）、全部已知枚举与标志位、
    // 以及帧头里那几份**重复元数据**（item_count/kind/alarm/failure）与解码出来的帧体是否一致。
    //
    // 绑定的生产者一旦退出（那个活动句柄已触发），这里一律返回 false：**绝不**再把最后一帧当成
    // 活的数据交出去。调用方据此清理任务栏组件，而不是继续画一份陈旧读数。
    // 序号一直不稳定、身份对不上、或任何检查不过时同样返回 false。
    bool read(Frame& out) const;

private:
    HANDLE mapping_ = nullptr;
    HANDLE ready_ = nullptr;
    HANDLE producer_ = nullptr;
    unsigned char const* view_ = nullptr;
    std::size_t capacity_ = 0;
    // attach 时从**活动句柄**记下的生产者身份。每次读都要拿帧头里的身份跟它比：帧头换了主人
    // （同名 section 被重建）时不能继续读。
    std::uint64_t owner_pid_ = 0;
    std::uint64_t owner_start_time_ = 0;
};

}  // namespace deskfeed
