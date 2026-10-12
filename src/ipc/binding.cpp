#include "ipc/binding.h"

#include "core/utf8.h"
#include "platform/win32_util.h"

namespace deskfeed {
namespace {

struct WaiterContext {
    HANDLE process = nullptr;
    HANDLE cancel = nullptr;
    HWND notify = nullptr;
    UINT message = 0;
    std::uint64_t generation = 0;
};

DWORD WINAPI waiter_entry(void* parameter) {
    // 只做等待与投递：真正的退出处理在主进程的消息线程上，按代次判定。
    auto* context = static_cast<WaiterContext*>(parameter);
    HANDLE handles[2] = {context->process, context->cancel};
    DWORD const wait = ::WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    if (wait == WAIT_OBJECT_0 && context->notify != nullptr) {
        ::PostMessageW(context->notify, context->message,
                       static_cast<WPARAM>(context->generation), 0);
    }
    delete context;
    return 0;
}

void close_waiter(HANDLE& cancel, HANDLE& thread) {
    if (cancel != nullptr) {
        ::SetEvent(cancel);
    }
    if (thread != nullptr) {
        ::WaitForSingleObject(thread, 5000);
        ::CloseHandle(thread);
        thread = nullptr;
    }
    if (cancel != nullptr) {
        ::CloseHandle(cancel);
        cancel = nullptr;
    }
}

}  // namespace

ProcessBinding::~ProcessBinding() { clear(); }

bool ProcessBinding::prepare(HANDLE process, unsigned long pid, std::uint64_t generation,
                             HWND notify, UINT message, Prepared& prepared, std::string& error) {
    prepared = Prepared{};
    if (process == nullptr) {
        error = "process handle is null";
        return false;
    }
    // 绑定前再看一眼：调用方验证过，这里再确认一次“还没退出”。
    if (::WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
        error = "the producer process has already exited";
        return false;
    }

    HANDLE const cancel = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (cancel == nullptr) {
        error = "CreateEvent failed: " + utf8_from_wide(last_error_text());
        return false;
    }
    auto* context = new WaiterContext{process, cancel, notify, message, generation};
    HANDLE const thread = ::CreateThread(nullptr, 0, &waiter_entry, context, 0, nullptr);
    if (thread == nullptr) {
        error = "CreateThread failed: " + utf8_from_wide(last_error_text());
        delete context;
        ::CloseHandle(cancel);
        return false;
    }

    prepared.process = process;
    prepared.cancel = cancel;
    prepared.thread = thread;
    prepared.pid = pid;
    prepared.generation = generation;
    prepared.armed = true;
    return true;
}

void ProcessBinding::discard(Prepared& prepared) {
    if (!prepared.armed) {
        return;
    }
    close_waiter(prepared.cancel, prepared.thread);
    if (prepared.process != nullptr) {
        ::CloseHandle(prepared.process);
        prepared.process = nullptr;
    }
    prepared.armed = false;
}

void ProcessBinding::commit(Prepared& prepared) {
    if (!prepared.armed) {
        return;
    }
    clear();
    process_ = prepared.process;
    cancel_ = prepared.cancel;
    thread_ = prepared.thread;
    pid_ = prepared.pid;
    generation_ = prepared.generation;
    prepared = Prepared{};
}

void ProcessBinding::clear() {
    close_waiter(cancel_, thread_);
    if (process_ != nullptr) {
        ::CloseHandle(process_);
        process_ = nullptr;
    }
    pid_ = 0;
    generation_ = 0;
}

bool ProcessBinding::signaled() const {
    if (process_ == nullptr) {
        return false;
    }
    return ::WaitForSingleObject(process_, 0) == WAIT_OBJECT_0;
}

}  // namespace deskfeed
