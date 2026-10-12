#include "payload/feed.h"

namespace deskfeed::payload {

FeedOutcome feed_decide(FeedInput const& input) {
    if (input.read_ok) {
        // 正常收尾：内容照旧，但生产者已经不打算再发了，块必须消失。
        return input.closed ? FeedOutcome::Remove : FeedOutcome::Apply;
    }

    // 生产者已经不在了（句柄触发或身份不再匹配）：手上的读数已经没有来源，摘块。
    if (!input.owner_alive) {
        return FeedOutcome::Remove;
    }

    // 生产者活着，这一轮没读出完整帧：可以沿用上一帧，但有界。
    if (input.have_last_frame && input.failures < kFeedMaxRetainedFailures) {
        return FeedOutcome::Keep;
    }
    return FeedOutcome::Remove;
}

}  // namespace deskfeed::payload
