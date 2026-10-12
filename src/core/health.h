#pragma once

#include <string>
#include <vector>

#include "core/snapshot.h"

// 健康、缓存与到期状态机（纯逻辑，不依赖 Windows）。
//
// 悬浮窗与任务栏显示面读的是同一份 View，所以“某一面正常、另一面失效”在结构上不可能发生。
// 规则来自 docs/protocol.md 的“失效、恢复与到期”：
//   * 失败一发生就进入整体失效：有条目就保留其值并全部染红，没有条目就显示红色 ERROR；
//   * 到期与通信失败同属整体失效，且由主进程按绝对时间独立判定，不依赖下一次轮询；
//   * 合法的 items: [] 立即清空内容，不是失败；清空之后再失败只能显示 ERROR，不找回旧值；
//   * 重新收到合法且未过期的快照后，恢复每一条自己的 state，而不是一律变绿。
namespace deskfeed {

enum class FailureKind {
    None,
    NoUrl,      // 没有配置 URL（启动时没有保存地址）
    Transport,  // 连不上、名字解析不了、连接中断
    Timeout,    // 超过整次取数的墙钟预算
    HttpStatus, // 上游返回非 200（含 204 / 304）
    Redirect,   // 重定向超次数或试图从 HTTPS 降级
    TooLarge,   // 正文超过 1 MiB
    Encoding,   // 正文不是合法 UTF-8
    Parse,      // 正文不是合法 JSON
    Validation, // 结构合法性问题（整份拒绝）
    Internal,   // 主进程自身的问题（保存失败之类）
};

char const* failure_kind_name(FailureKind kind);

enum class ViewKind {
    Waiting,  // 首次请求等待中：不显示读数，也不显示 ERROR
    Content,  // 有条目（alarm 为真时全部按失效色渲染）
    Error,    // 没有可显示的条目，只能显示红色 ERROR + 原因
};

struct ViewItem {
    std::string id;
    std::string label;
    std::string value;
    ItemState state = ItemState::Ok;
    std::string detail;
};

struct View {
    ViewKind kind = ViewKind::Waiting;
    std::vector<ViewItem> items;
    bool alarm = false;  // 整体失效：所有条目的值都要按失效色渲染
    FailureKind failure = FailureKind::None;
    std::string reason;
};

class HealthState {
public:
    HealthState();

    // 数据源是否已配置。true 表示进入“等待首份快照”，false 表示“未配置 URL”。
    // 切换 URL 时调用它清空旧源缓存（协议要求换源清缓存）。
    void set_configured(bool configured);

    // 一份已经通过校验、被接纳的快照。
    void apply_snapshot(Snapshot const& snapshot, long long now_ms);

    // 一次取数或校验失败（失败一发生就进入整体失效）。
    void apply_failure(FailureKind kind, std::string reason, long long now_ms);

    // 到点检查：只根据绝对到期时间把状态切成过期，不依赖轮询响应。
    void tick(long long now_ms);

    bool has_snapshot() const { return has_snapshot_; }
    bool expired() const { return expired_; }
    bool alarm() const { return failure_ != FailureKind::None || expired_; }
    FailureKind failure() const { return failure_; }
    std::string const& reason() const { return reason_; }
    long long received_at_ms() const { return received_at_ms_; }
    Snapshot const& snapshot() const { return snapshot_; }

    // 下一次需要唤醒的时间（到期时刻）；-1 表示当前没有可等待的到期。
    long long next_expiry_ms() const;

    View view() const;

private:
    enum class Phase { Unconfigured, Waiting, Data };

    void clear_content();

    Phase phase_ = Phase::Unconfigured;
    Snapshot snapshot_{};
    bool has_snapshot_ = false;
    bool expired_ = false;
    FailureKind failure_ = FailureKind::NoUrl;
    std::string reason_;
    long long received_at_ms_ = 0;
};

}  // namespace deskfeed
