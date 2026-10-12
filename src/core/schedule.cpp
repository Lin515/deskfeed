#include "core/schedule.h"

namespace deskfeed {
namespace {

ScheduleClamp clamp_value(long long value, long long minimum, long long maximum,
                          long long fallback) {
    ScheduleClamp result;
    if (value <= 0) {
        result.value = fallback;
        result.used_default = true;
        return result;
    }
    if (value < minimum) {
        result.value = minimum;
        result.clamped = true;
        return result;
    }
    if (value > maximum) {
        result.value = maximum;
        result.clamped = true;
        return result;
    }
    result.value = value;
    return result;
}

}  // namespace

ScheduleBounds const& schedule_bounds() {
    static ScheduleBounds const bounds;
    return bounds;
}

ScheduleClamp schedule_clamp_poll(long long value) {
    return clamp_value(value, schedule_bounds().poll_min, schedule_bounds().poll_max, 30000);
}

ScheduleClamp schedule_clamp_retry(long long value) {
    return clamp_value(value, schedule_bounds().retry_min, schedule_bounds().retry_max, 5000);
}

ScheduleClamp schedule_clamp_timeout(long long value) {
    return clamp_value(value, schedule_bounds().timeout_min, schedule_bounds().timeout_max, 8000);
}

void FetchScheduler::on_result(bool ok, long long now_ms) {
    long long const delay = ok ? config_.poll_ms : config_.retry_ms;
    due_ms_ = now_ms + delay;
}

}  // namespace deskfeed
