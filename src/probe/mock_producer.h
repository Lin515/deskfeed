#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/health.h"
#include "display/frame.h"
#include "display/mailbox.h"
#include "probe/scope_claim.h"

// 探针自己的**隔离显示帧生产者**。
//
// 负载的硬边界是"不做外部 HTTP、不解析 JSON、只读主进程发布的最新完整帧"。所以自测宿主也不许
// 让负载去取数：宿主自己起一个 `DisplayFrameProducer`（用一个隔离 scope），把合成帧发进去，
// 负载从**同一块共享内存**读。这样"帧 → 块"那条路走的是生产路径，而不是把内容直接塞进负载。
//
// 建之前**必须**先用 `ProbeScopeClaim` 抢到实例仲裁名字（见 probe/scope_claim.h）：抢不到就
// 说明这个 scope 已被主进程或另一个探针占着，此时 `DisplayFrameProducer::create` 会把别人的
// 帧头 memset 掉（覆写邮箱），所以只能拒绝。
namespace deskfeed::probe {

class MockProducer {
public:
    MockProducer() = default;
    ~MockProducer();
    MockProducer(MockProducer const&) = delete;
    MockProducer& operator=(MockProducer const&) = delete;

    // `scope` 必须是一个隔离实例名（探针默认 `probe`），绝不碰生产命名空间。
    //
    // **占用即拒绝**：同名显示邮箱已经存在（真实主进程，或另一个探针）时返回 false，绝不 open、
    // 更不覆写它。依据是 `DisplayFrameProducer::create` 对已存在的同名 section **照样成功**，并在
    // 建视图后把帧头 `memset` 成零（见 display/mailbox.cpp），也就是把别人的邮箱头清掉、再写上自己
    // 的身份。所以"有没有人占着"必须由调用方在 create 之前判定。
    bool create(std::wstring const& scope, std::string& error);
    void close();
    bool created() const { return created_; }

    // 发一帧。`now_ms` 用真实墙钟毫秒（帧头里的 produced_at_ms 是给人看的）。
    bool publish(Frame const& frame);
    // 正常收尾：保留最后一帧、置 closed 标志并唤醒读端。
    bool publish_closed();

    std::wstring const& scope() const { return scope_; }
    std::uint64_t generation() const { return generation_; }

private:
    bool write(Frame const& frame, bool closed);

    bool created_ = false;
    std::wstring scope_;
    void* producer_ = nullptr;  // 实现细节（DisplayFrameProducer）藏在 .cpp 里
    ProbeScopeClaim claim_;     // 实例仲裁（与主进程同一个名字）；close() 时归还
    std::uint64_t generation_ = 0;
};

// 合成一帧的几种形状（都只用通用条目字段，没有任何业务语义）。
Frame make_content_frame(std::vector<FrameItem> items, bool alarm = false,
                         FailureKind failure = FailureKind::None, std::string reason = {});
Frame make_waiting_frame();
Frame make_error_frame(FailureKind failure, std::string reason);
FrameItem make_item(std::string id, std::string label, std::string value,
                    ItemState state = ItemState::Ok, std::string detail = {});

}  // namespace deskfeed::probe
