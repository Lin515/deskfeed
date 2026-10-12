#include "core/health.h"

#include <utility>

namespace deskfeed {

char const* failure_kind_name(FailureKind kind) {
    switch (kind) {
        case FailureKind::None: return "none";
        case FailureKind::NoUrl: return "no-url";
        case FailureKind::Transport: return "transport";
        case FailureKind::Timeout: return "timeout";
        case FailureKind::HttpStatus: return "http-status";
        case FailureKind::Redirect: return "redirect";
        case FailureKind::TooLarge: return "too-large";
        case FailureKind::Encoding: return "encoding";
        case FailureKind::Parse: return "parse";
        case FailureKind::Validation: return "validation";
        case FailureKind::Internal: return "internal";
    }
    return "unknown";
}

HealthState::HealthState() {
    reason_ = "未配置 URL";
}

void HealthState::clear_content() {
    snapshot_ = Snapshot{};
    has_snapshot_ = false;
    expired_ = false;
    received_at_ms_ = 0;
}

void HealthState::set_configured(bool configured) {
    clear_content();
    if (configured) {
        phase_ = Phase::Waiting;
        failure_ = FailureKind::None;
        reason_.clear();
    } else {
        phase_ = Phase::Unconfigured;
        failure_ = FailureKind::NoUrl;
        reason_ = "未配置 URL";
    }
}

void HealthState::apply_snapshot(Snapshot const& snapshot, long long now_ms) {
    snapshot_ = snapshot;
    has_snapshot_ = true;
    received_at_ms_ = now_ms;
    // 首份快照即使已过期也要接纳其内容：条目会在 View 里全部按失效色渲染。
    expired_ = snapshot_.expired_at(now_ms);
    phase_ = Phase::Data;
    failure_ = FailureKind::None;
    reason_.clear();
}

void HealthState::apply_failure(FailureKind kind, std::string reason, long long now_ms) {
    (void)now_ms;  // 失败不改写“收到快照的绝对时间”，过期仍按快照自己的 expiresAtMs 判断。
    failure_ = kind == FailureKind::None ? FailureKind::Internal : kind;
    reason_ = std::move(reason);
}

void HealthState::tick(long long now_ms) {
    if (has_snapshot_ && !expired_ && snapshot_.has_expires_at && now_ms >= snapshot_.expires_at_ms) {
        expired_ = true;
    }
}

long long HealthState::next_expiry_ms() const {
    if (!has_snapshot_ || expired_ || !snapshot_.has_expires_at) {
        return -1;
    }
    return snapshot_.expires_at_ms;
}

View HealthState::view() const {
    View view;
    bool const failed = failure_ != FailureKind::None;

    if (phase_ == Phase::Unconfigured) {
        view.kind = ViewKind::Error;
        view.alarm = true;
        view.failure = FailureKind::NoUrl;
        view.reason = reason_.empty() ? std::string("未配置 URL") : reason_;
        return view;
    }

    if (!snapshot_.items.empty()) {
        view.kind = ViewKind::Content;
        view.items.reserve(snapshot_.items.size());
        for (SnapshotItem const& item : snapshot_.items) {
            ViewItem out;
            out.id = item.id;
            out.label = item.label;
            out.value = item.value;
            out.state = item.state;
            out.detail = item.detail;
            view.items.push_back(std::move(out));
        }
        view.alarm = failed || expired_;
        view.failure = failed ? failure_ : FailureKind::None;
        // 过期是有值的整体失效：读者需要一句能看见的原因（tooltip / 状态），但不能改写生产端文案。
        // 健康内容不带任何 reason，别让“最近一次失败的原因”漏到正常读数上。
        if (failed) {
            view.reason = reason_;
        } else if (expired_) {
            view.reason = "快照已过期";
        }
        return view;
    }

    if (failed || expired_) {
        view.kind = ViewKind::Error;
        view.alarm = true;
        view.failure = failed ? failure_ : FailureKind::None;
        if (failed) {
            view.reason = reason_;
        } else {
            view.reason = "快照已过期";
        }
        return view;
    }

    if (phase_ == Phase::Waiting) {
        view.kind = ViewKind::Waiting;
        return view;
    }

    // 合法的空 items：内容已按协议清空，不是失败。
    view.kind = ViewKind::Content;
    return view;
}

}  // namespace deskfeed
