#include "probe/mock_producer.h"

#include "display/mailbox.h"

namespace deskfeed::probe {

MockProducer::~MockProducer() { close(); }

bool MockProducer::create(std::wstring const& scope, std::string& error) {
    close();
    auto* producer = new (std::nothrow) DisplayFrameProducer();
    if (producer == nullptr) {
        error = "cannot allocate the display frame producer";
        return false;
    }
    if (!producer->create(scope, nullptr, error)) {
        delete producer;
        return false;
    }
    producer_ = producer;
    scope_ = scope;
    created_ = true;
    return true;
}

void MockProducer::close() {
    if (producer_ != nullptr) {
        delete static_cast<DisplayFrameProducer*>(producer_);
        producer_ = nullptr;
    }
    created_ = false;
}

bool MockProducer::write(Frame const& frame, bool closed) {
    if (producer_ == nullptr) {
        return false;
    }
    auto* producer = static_cast<DisplayFrameProducer*>(producer_);
    FILETIME stamp{};
    ::GetSystemTimeAsFileTime(&stamp);
    ULARGE_INTEGER value{};
    value.LowPart = stamp.dwLowDateTime;
    value.HighPart = stamp.dwHighDateTime;
    long long const now_ms = static_cast<long long>(value.QuadPart / 10000ull) - 11644473600000LL;
    bool const ok = closed ? producer->publish_closed(now_ms) : producer->publish(frame, now_ms);
    if (ok) {
        ++generation_;
    }
    return ok;
}

bool MockProducer::publish(Frame const& frame) { return write(frame, false); }

bool MockProducer::publish_closed() { return write(Frame{}, true); }

FrameItem make_item(std::string id, std::string label, std::string value, ItemState state,
                    std::string detail) {
    FrameItem item;
    item.id = std::move(id);
    item.label = std::move(label);
    item.value = std::move(value);
    item.state = state;
    item.detail = std::move(detail);
    return item;
}

Frame make_content_frame(std::vector<FrameItem> items, bool alarm, FailureKind failure,
                         std::string reason) {
    Frame frame;
    frame.kind = ViewKind::Content;
    frame.items = std::move(items);
    frame.alarm = alarm;
    frame.failure = failure;
    frame.reason = std::move(reason);
    return frame;
}

Frame make_waiting_frame() {
    Frame frame;
    frame.kind = ViewKind::Waiting;
    return frame;
}

Frame make_error_frame(FailureKind failure, std::string reason) {
    Frame frame;
    frame.kind = ViewKind::Error;
    frame.failure = failure;
    frame.reason = std::move(reason);
    return frame;
}

}  // namespace deskfeed::probe
