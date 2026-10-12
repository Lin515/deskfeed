#pragma once

// 显示帧的**消费策略**：只有一份纯逻辑，不碰 Windows / 共享内存 / XAML，所以它可以离线
// 逐分支测（`tests/native_tests.cpp`）。
//
// 这套策略回答的是负载运行期唯一的那个问题：“这一轮读到的邮箱状态，该把任务栏那块怎么处理”。
// 三条硬约束（见 docs/taskbar-injection.md）：
//   * 主进程**正常退出**（帧里带 closed 标志）→ 立刻把块摘掉，不保留任何旧读数；
//   * 主进程**死亡**（绑定的生产者进程句柄已触发，或邮箱拒绝再读）→ 同样摘掉，**绝不**把最后
//     一帧当成活数据继续画（这正是“master 已死却还在画陈旧读数”的正面修复）；
//   * 主进程**活着**、但这一轮读到的帧坏了（写到一半、校验不过、身份对不上）→ 可以继续沿用
//     上一次**完整**的帧，但有界：连续失败到上限就放弃，把块摘掉，避免永远停在旧读数上。
namespace deskfeed::payload {

// 连续多少轮“生产者活着但帧读不出来”之后不再沿用上一帧。取值理由：负载的空转周期是
// 250 ms 一档，3 轮 ≈ 1 秒，足够跨过一次正在进行的提交，又不至于让用户看着一份坏掉之后
// 再也不更新的读数。
inline constexpr unsigned kFeedMaxRetainedFailures = 3;

enum class FeedOutcome {
    Apply,  // 用这一帧的内容（就地换字，或在块不在树里时补插）
    Keep,   // 什么都不做：沿用上一次完整的帧
    Remove, // 把块摘掉（正常收尾、生产者死亡、或坏帧超过上限）
};

struct FeedInput {
    bool read_ok = false;         // 本轮 read() 是否成功
    bool closed = false;          // 成功读到的帧是否带 closed 标志（read_ok 为真时才有意义）
    bool owner_alive = false;     // 绑定的生产者进程是否还活着
    bool have_last_frame = false; // 手上是否已经有一份完整、已应用的帧
    unsigned failures = 0;        // 进入本轮之前，连续“生产者活着但读不出帧”的轮数
};

// 纯决策。调用方负责在 Apply 之后把 last_frame 更新成新帧、在 Keep 时保留旧帧、在任何一次
// 成功的 read 之后把 failures 清零。
FeedOutcome feed_decide(FeedInput const& input);

}  // namespace deskfeed::payload
