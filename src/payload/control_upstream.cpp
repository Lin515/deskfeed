#include "payload/control_upstream.h"

#include "core/utf8.h"
#include "ipc/control_wire.h"
#include "platform/paths.h"
#include "platform/win32_util.h"

#include <windows.h>

namespace deskfeed::payload {
namespace {

// 有界：这是任务栏线程派出来的工作线程在等，主进程再忙也不能把它挂住太久。
constexpr unsigned kControlSendTimeoutMs = 1500;

bool send_op(std::wstring const& scope, ControlOp op, std::vector<std::wstring> const& args,
             unsigned timeout_ms, std::string& error) {
    std::wstring const class_name = scoped_name(kControlWindowClassBase, scope);
    HWND const window = ::FindWindowW(class_name.c_str(), nullptr);
    if (window == nullptr) {
        error = "主进程没有在监听（找不到控制窗口）";
        return false;
    }

    ControlRequest request;
    request.op = op;
    request.sender_pid = ::GetCurrentProcessId();
    request.args = args;
    std::string const payload = control_encode(request);
    if (payload.size() > kControlMaxPayloadBytes) {
        error = "控制载荷过大";
        return false;
    }

    COPYDATASTRUCT data{};
    data.dwData = kControlMagic;
    data.cbData = static_cast<DWORD>(payload.size());
    data.lpData = const_cast<char*>(payload.data());

    DWORD_PTR result = 0;
    ::SetLastError(ERROR_SUCCESS);
    // SMTO_ABORTIFHUNG：主进程卡住时立刻失败，不把工作线程挂在这里。
    LRESULT const sent = ::SendMessageTimeoutW(window, WM_COPYDATA, 0,
                                               reinterpret_cast<LPARAM>(&data),
                                               SMTO_ABORTIFHUNG | SMTO_BLOCK, timeout_ms, &result);
    if (sent == 0) {
        DWORD const code = ::GetLastError();
        error = code == ERROR_TIMEOUT ? "主进程没有在期限内应答"
                                      : "SendMessageTimeout 失败: " + utf8_from_wide(last_error_text(code));
        return false;
    }
    auto const reply = static_cast<ControlReply>(static_cast<std::intptr_t>(result));
    if (reply != ControlReply::Accepted) {
        error = "主进程没有接受这个动作";
        return false;
    }
    return true;
}

}  // namespace

bool control_request_refresh(std::wstring const& scope, unsigned timeout_ms, std::string& error) {
    return send_op(scope, ControlOp::Refresh, {}, timeout_ms, error);
}

bool control_request_show_widget(std::wstring const& scope, unsigned timeout_ms, std::string& error) {
    return send_op(scope, ControlOp::ShowWidget, {}, timeout_ms, error);
}

bool control_request_set_screens(std::wstring const& scope,
                                 std::vector<std::wstring> const& screen_ids, unsigned timeout_ms,
                                 std::string& error) {
    return send_op(scope, ControlOp::SetScreens, screen_ids, timeout_ms, error);
}

}  // namespace deskfeed::payload
