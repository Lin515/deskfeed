#pragma once

#include <string>

// 取数调度（纯逻辑）。默认值来自 docs/protocol.md 的“HTTP 规则与调度”：
// 正常轮询 30000 ms、失败重试 5000 ms、请求超时 8000 ms。
//
// 秒级轮询是协议明确要求的能力（不延续旧任务栏的 60000 ms 下限），所以轮询下限取 1000 ms。
// 上下限属于实现细则，写在 docs/configuration.md 里，随本文件一起修改。
namespace deskfeed {

struct ScheduleConfig {
    long long poll_ms = 30000;
    long long retry_ms = 5000;
    long long timeout_ms = 8000;
};

struct ScheduleBounds {
    long long poll_min = 1000;
    long long poll_max = 24LL * 60 * 60 * 1000;   // 24 小时
    long long retry_min = 1000;
    long long retry_max = 60LL * 60 * 1000;      // 1 小时
    long long timeout_min = 1000;
    long long timeout_max = 60LL * 1000;         // 1 分钟
};

ScheduleBounds const& schedule_bounds();

// 把配置值收敛到合法区间：0 或负数表示“没有配置”，用默认值；
// 超出上下限的值夹到边界。changed 记录是否被改动过（供日志说明）。
struct ScheduleClamp {
    long long value = 0;
    bool used_default = false;
    bool clamped = false;
};

ScheduleClamp schedule_clamp_poll(long long value);
ScheduleClamp schedule_clamp_retry(long long value);
ScheduleClamp schedule_clamp_timeout(long long value);

// 一次取数的排程：成功后等 poll_ms，失败后等 retry_ms。
class FetchScheduler {
public:
    void set_config(ScheduleConfig const& config) { config_ = config; }
    ScheduleConfig const& config() const { return config_; }

    void reset(long long now_ms) { due_ms_ = now_ms; }
    void on_result(bool ok, long long now_ms);
    long long due_ms() const { return due_ms_; }

private:
    ScheduleConfig config_{};
    long long due_ms_ = 0;
};

}  // namespace deskfeed
