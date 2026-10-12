#include "master/master.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "core/health.h"
#include "core/schedule.h"
#include "core/snapshot.h"
#include "core/utf8.h"
#include "display/frame.h"
#include "display/mailbox.h"
#include "ipc/binding.h"
#include "ipc/command.h"
#include "ipc/control_channel.h"
#include "ipc/settings.h"
#include "net/http_client.h"
#include "net/url.h"
#include "platform/console.h"
#include "platform/log.h"
#include "platform/paths.h"
#include "platform/user_scope.h"
#include "platform/win32_util.h"

namespace deskfeed {
namespace {

constexpr UINT kMsgFetchDone = WM_APP + 1;   // lParam = FetchOutcome*
constexpr UINT kMsgBindingExit = WM_APP + 2; // wParam = 绑定代次
constexpr UINT kMsgQuit = WM_APP + 3;
constexpr unsigned kControlSendTimeoutMs = 3000;
constexpr unsigned kShutdownJoinMs = 2000;
// 同时在跑的“有效”取数上限。取消现在是真的（HTTP 层到点或被要求就关掉在途请求句柄），
// 所以一个被放弃的作业只占几十毫秒；它不再挡着新源的首取——名额按 active 计数，
// 一取消就释放。kMaxFetchJobs 是作业记录的硬上限，只用来兜住极端情况下的堆积。
constexpr std::size_t kMaxLiveFetches = 2;
constexpr std::size_t kMaxFetchJobs = 8;

long long monotonic_ms() { return static_cast<long long>(::GetTickCount64()); }

// 健康/到期用的是快照里的绝对时间（Unix 毫秒），所以这一层必须有真实的墙钟；
// 调度与超时用单调时钟（monotonic_ms），两者不能混用，也不能互相换算。
long long wall_ms() {
    FILETIME file_time{};
    ::GetSystemTimeAsFileTime(&file_time);
    ULARGE_INTEGER value{};
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    return static_cast<long long>(value.QuadPart / 10000ULL) - 11644473600000LL;
}

std::wstring redact(std::wstring const& url) {
    bool removed = false;
    std::wstring const out = url_redact(url, &removed);
    return out;
}

std::string json_escape(std::string const& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char const byte : text) {
        switch (byte) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (byte < 0x20) {
                    char buffer[8]{};
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", byte);
                    out += buffer;
                } else {
                    out.push_back(static_cast<char>(byte));
                }
        }
    }
    return out;
}

std::string json_string(std::string const& text) { return "\"" + json_escape(text) + "\""; }

std::string json_wstring(std::wstring const& text) { return json_string(utf8_from_wide(text)); }

struct FetchOutcome {
    std::uint64_t job_id = 0;
    std::uint64_t generation = 0;
    HttpResult http;
    ValidationResult validation;
    Snapshot snapshot;
};

struct FetchJobContext {
    std::uint64_t job_id = 0;
    std::uint64_t generation = 0;
    std::wstring url;
    HttpLimits limits;
    std::shared_ptr<std::atomic<bool>> abort;
    HWND notify = nullptr;
};

DWORD WINAPI fetch_thread_entry(void* parameter) {
    std::unique_ptr<FetchJobContext> context(static_cast<FetchJobContext*>(parameter));
    auto outcome = std::make_unique<FetchOutcome>();
    outcome->job_id = context->job_id;
    outcome->generation = context->generation;
    outcome->http = http_get(context->url, context->limits, context->abort.get());
    if (outcome->http.ok) {
        outcome->validation = snapshot_validate(outcome->http.body, outcome->snapshot);
    }
    // 投递给主进程；投递成功就把所有权交出去，否则随 unique_ptr 一起释放。
    if (::PostMessageW(context->notify, kMsgFetchDone, 0,
                       reinterpret_cast<LPARAM>(outcome.get())) != FALSE) {
        outcome.release();
    }
    return 0;
}

FailureKind failure_from_http(HttpFailure failure) {
    switch (failure) {
        case HttpFailure::InvalidUrl: return FailureKind::Transport;
        case HttpFailure::Connect: return FailureKind::Transport;
        case HttpFailure::Timeout: return FailureKind::Timeout;
        case HttpFailure::Send: return FailureKind::Transport;
        case HttpFailure::Receive: return FailureKind::Transport;
        case HttpFailure::Redirect: return FailureKind::Redirect;
        case HttpFailure::Status: return FailureKind::HttpStatus;
        case HttpFailure::TooLarge: return FailureKind::TooLarge;
        case HttpFailure::Empty: return FailureKind::Validation;
        case HttpFailure::Aborted: return FailureKind::None;
        case HttpFailure::None: return FailureKind::None;
    }
    return FailureKind::Transport;
}

FailureKind failure_from_validation(ValidationKind kind) {
    switch (kind) {
        case ValidationKind::Encoding: return FailureKind::Encoding;
        case ValidationKind::TooLarge: return FailureKind::TooLarge;
        case ValidationKind::Parse: return FailureKind::Parse;
        default: return FailureKind::Validation;
    }
}

// View → Frame：显示面要的是同一份模型，只是换成能跨进程传的有界线格式。
// 只搬字段，不做任何解释——负载 DLL 也照这份线格式读，两边不可能对同一份快照有两种读法。
Frame frame_from_view(View const& view) {
    Frame frame;
    frame.kind = view.kind;
    frame.alarm = view.alarm;
    frame.failure = view.failure;
    frame.reason = view.reason;
    frame.items.reserve(view.items.size());
    for (ViewItem const& item : view.items) {
        FrameItem out;
        out.id = item.id;
        out.label = item.label;
        out.value = item.value;
        out.state = item.state;
        out.detail = item.detail;
        frame.items.push_back(std::move(out));
    }
    return frame;
}

class Master {
public:
    Master(Command command, std::wstring scope)
        : command_(std::move(command)), scope_(std::move(scope)) {
        no_taskbar_ = command_.no_taskbar;
        hidden_ = command_.hidden;
    }
    ~Master();

    int run();

private:
    struct Job {
        std::uint64_t id = 0;
        std::uint64_t generation = 0;
        HANDLE thread = nullptr;
        bool active = false;
        bool timed_out = false;
        std::shared_ptr<std::atomic<bool>> abort;
    };

    bool initialize();
    ControlReply on_control(ControlRequest const& request);
    ControlReply apply_command(Command const& command, std::uint32_t sender_pid);
    void start_job();
    void cancel_active_job();
    void reap_jobs();
    Job* find_job(std::uint64_t id);
    void on_fetch_done(std::unique_ptr<FetchOutcome> outcome);
    void on_binding_exit(std::uint64_t generation);
    // 把当前显示模型发布进显示邮箱。force 用于“模型没变但读端需要一帧”的场合
    // （启动时、任务栏组件被重新打开/重新注入时）。
    void publish_display_frame(bool force);
    void shutdown();
    std::string inspect_json() const;

    Command command_;
    std::wstring scope_;
    std::wstring class_name_;
    std::wstring settings_path_;
    std::wstring log_path_;
    ControlServer server_;
    std::unique_ptr<SettingsFile> settings_;
    bool settings_usable_ = true;

    ScheduleConfig schedule_{};
    std::wstring url_;
    bool configured_ = false;
    HealthState health_;
    ProcessBinding binding_;
    FetchScheduler scheduler_;

    std::vector<Job> jobs_;
    std::uint64_t next_job_id_ = 1;
    std::uint64_t active_job_id_ = 0;
    long long fetch_deadline_ms_ = 0;

    std::uint64_t source_generation_ = 0;
    std::uint64_t binding_generation_ = 0;
    // 绑定代次的分配计数器：**从不复用**。prepare 会立刻启动等待者，所以一次最终没有提交的
    // 尝试（落盘失败被 discard）也可能已经把自己的代次投递进消息队列，而 discard 收不回
    // 已排队的窗口消息。用 binding_generation_ + 1 的话，下一次成功的绑定会拿到同一个值，
    // 那条迟到的旧消息就会误伤新绑定。
    std::uint64_t next_binding_generation_ = 1;
    bool quit_requested_ = false;
    bool no_taskbar_ = false;
    bool hidden_ = false;

    // 显示邮箱：只有显式要求（--frame-mailbox）时才建。建起来之后它是主进程 → 显示面
    // 唯一的输出通道；发布失败只记日志，绝不影响取数。
    DisplayFrameProducer frame_producer_;
    std::uint64_t published_revision_ = 0;
};

Master::~Master() {
    for (Job& job : jobs_) {
        if (job.thread != nullptr) {
            if (job.abort) {
                job.abort->store(true);
            }
            ::WaitForSingleObject(job.thread, kShutdownJoinMs);
            ::CloseHandle(job.thread);
            job.thread = nullptr;
        }
    }
    jobs_.clear();
}

bool Master::initialize() {
    class_name_ = scoped_name(kControlWindowClassBase, scope_);
    settings_path_ = command_.state_dir.empty() ? deskfeed_settings_path(scope_)
                                                : command_.state_dir + L"\\settings.ini";
    log_path_ = deskfeed_log_path(scope_);

    // 顺序按契约：先仲裁（已在 master_main 完成）与接收端就绪，再截断日志、再动网络。
    std::wstring directory_error;
    if (!ensure_parent_directory(log_path_, directory_error)) {
        console_write_stderr(L"deskfeed: " + directory_error + L"\n");
        return false;
    }
    std::string error;
    if (!server_.create(class_name_, nullptr, error)) {
        console_write_stderr(L"deskfeed: 建不了控制窗口：" + wide_from_utf8(error) + L"\n");
        return false;
    }
    server_.set_handler([this](ControlRequest const& request) { return on_control(request); });

    if (!log_open(log_path_, false)) {
        console_write_stderr(L"deskfeed: 建不了日志文件 " + log_path_ + L"\n");
        return false;
    }
    log_format(L"deskfeed: master starting (pid=%lu scope=%s class=%s)",
               ::GetCurrentProcessId(), scope_.empty() ? L"(production)" : scope_.c_str(),
               class_name_.c_str());

    settings_ = std::make_unique<SettingsFile>(settings_path_);
    std::wstring settings_error;
    if (!settings_->load(settings_error)) {
        settings_usable_ = false;
        log_line(L"settings: " + settings_error + L"；本次运行不会写配置");
    }
    if (no_taskbar_) {
        log_line(L"deskfeed: --no-inject 生效：本次运行不碰任务栏");
    }
    if (hidden_) {
        log_line(L"deskfeed: --hidden 生效：本次运行悬浮窗保持隐藏");
    }
    if (command_.frame_mailbox) {
        // 显式要求了这条 seam，建不起来就是失败：静默继续会让离线验收“跑通了但什么也没验”。
        // 它只建一块按 scope 命名的共享内存与一个通知事件，不碰界面、不注入。
        UserSecurity security;
        if (!security.build()) {
            log_line(L"display: 建不了用户安全描述符，显示邮箱改用系统默认 ACL");
        }
        std::string mailbox_error;
        if (!frame_producer_.create(scope_, &security, mailbox_error)) {
            std::wstring const message = wide_from_utf8(mailbox_error);
            console_write_stderr(L"deskfeed: 建不了显示邮箱：" + message + L"\n");
            log_line(L"display: 建不了显示邮箱：" + message);
            return false;
        }
        log_line(L"display: 显示邮箱已建立（--frame-mailbox：只建隔离的命名对象，不碰界面）");
    }
    log_line(L"deskfeed: 任务栏负载尚未实现（Phase 2）；主进程当前做取数、状态机与显示帧发布");
    return true;
}

void Master::publish_display_frame(bool force) {
    if (!frame_producer_.created()) {
        return;
    }
    std::uint64_t const revision = health_.revision();
    if (!force && revision == published_revision_) {
        return;  // 模型没变：不往跨进程 seqlock 里白写一帧（一次一兆多字节）
    }
    Frame const frame = frame_from_view(health_.view());
    if (frame_producer_.publish(frame, wall_ms())) {
        published_revision_ = revision;
    } else {
        log_line(L"display: 发布显示帧失败（不影响取数）");
    }
}

ControlReply Master::on_control(ControlRequest const& request) {
    if (request.op == ControlOp::Inspect) {
        std::string const json = inspect_json();
        std::wstring const path = deskfeed_inspect_path(scope_, request.sender_pid);
        std::wstring error;
        if (!write_file_bytes(path, json, error)) {
            log_line(L"inspect: " + error);
            return ControlReply::Rejected;
        }
        log_format(L"inspect: wrote %s (%zu bytes)", path.c_str(), json.size());
        return ControlReply::Accepted;
    }

    Command command;
    std::wstring error;
    if (!command_parse(request.args, command, error)) {
        log_line(L"control: 参数不合法：" + error);
        return ControlReply::BadRequest;
    }
    switch (command.kind) {
        case CommandKind::QuitIfBound:
            if (!binding_.active() || binding_.pid() != command.pid) {
                log_format(L"control: --quit-if-bound %lu 不匹配（当前绑定 %s）", command.pid,
                           binding_.active() ? std::to_wstring(binding_.pid()).c_str() : L"无");
                return ControlReply::NotBound;
            }
            log_format(L"control: --quit-if-bound %lu 匹配；准备正常退出", command.pid);
            ::PostMessageW(server_.window(), kMsgQuit, 0, 0);
            return ControlReply::Accepted;
        case CommandKind::Quit:
            log_line(L"control: --quit；准备正常退出");
            ::PostMessageW(server_.window(), kMsgQuit, 0, 0);
            return ControlReply::Accepted;
        case CommandKind::Apply:
            return apply_command(command, request.sender_pid);
        case CommandKind::Start:
            log_line(L"control: 无参数命令：不改变地址与绑定");
            return ControlReply::Accepted;
        case CommandKind::Inspect:
        case CommandKind::Help:
            return ControlReply::BadRequest;
    }
    return ControlReply::BadRequest;
}

ControlReply Master::apply_command(Command const& command, std::uint32_t sender_pid) {
    // 1. 地址
    UrlParts parts;
    std::wstring url_error;
    if (!url_crack(command.url, parts, url_error)) {
        log_line(L"apply: 地址被拒绝：" + url_error);
        return ControlReply::Rejected;
    }

    // 2. 生产者句柄（准备阶段：失败时不碰旧绑定）
    unsigned long const pid = command.has_pid ? command.pid : 0;
    std::uint64_t const next_binding_generation = next_binding_generation_++;
    ProcessBinding::Prepared prepared;
    if (command.has_pid) {
        if (pid == 0 || pid == 4 || pid == ::GetCurrentProcessId() ||
            (sender_pid != 0 && pid == sender_pid)) {
            log_format(L"apply: PID %lu 不合法（0 / System / 本进程 / 转发进程都不接受）", pid);
            return ControlReply::Rejected;
        }
        HANDLE const process = ::OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (process == nullptr) {
            log_format(L"apply: 打不开 PID %lu：%s", pid, last_error_text().c_str());
            return ControlReply::Rejected;
        }
        std::string binding_error;
        if (!ProcessBinding::prepare(process, pid, next_binding_generation, server_.window(),
                                     kMsgBindingExit, prepared, binding_error)) {
            ::CloseHandle(process);  // 失败时句柄仍归调用方
            log_format(L"apply: 绑定 PID %lu 失败：%s", pid, wide_from_utf8(binding_error).c_str());
            return ControlReply::Rejected;
        }
    }

    // 3. 落盘（一次原子替换；失败则放弃准备阶段，旧绑定与旧运行时状态不变）
    bool changed = false;
    std::wstring settings_error;
    if (!settings_usable_) {
        ProcessBinding::discard(prepared);
        log_line(L"apply: 设置文件不可用，拒绝整条命令");
        return ControlReply::Rejected;
    }
    settings_->set(L"source", L"url", command.url, &changed, settings_error);
    if (command.has_poll_ms) {
        settings_->set(L"source", L"poll_ms",
                       std::to_wstring(schedule_clamp_poll(command.poll_ms).value), &changed,
                       settings_error);
    }
    if (command.has_retry_ms) {
        settings_->set(L"source", L"retry_ms",
                       std::to_wstring(schedule_clamp_retry(command.retry_ms).value), &changed,
                       settings_error);
    }
    if (command.has_timeout_ms) {
        settings_->set(L"source", L"timeout_ms",
                       std::to_wstring(schedule_clamp_timeout(command.timeout_ms).value), &changed,
                       settings_error);
    }
    if (!settings_->save(settings_error)) {
        ProcessBinding::discard(prepared);
        log_line(L"apply: 保存设置失败：" + settings_error + L"；拒绝整条接管");
        std::wstring reload_error;
        settings_->load(reload_error);  // 把内存里的脏改动丢回磁盘状态
        return ControlReply::Rejected;
    }

    // 4. 提交运行时状态
    ScheduleConfig next_schedule = schedule_;
    if (command.has_poll_ms) {
        next_schedule.poll_ms = schedule_clamp_poll(command.poll_ms).value;
    }
    if (command.has_retry_ms) {
        next_schedule.retry_ms = schedule_clamp_retry(command.retry_ms).value;
    }
    if (command.has_timeout_ms) {
        next_schedule.timeout_ms = schedule_clamp_timeout(command.timeout_ms).value;
    }

    bool const url_changed = command.url != url_;
    schedule_ = next_schedule;
    scheduler_.set_config(schedule_);
    binding_generation_ = next_binding_generation;
    if (command.has_pid) {
        binding_.commit(prepared);
    } else {
        binding_.clear();
    }

    if (url_changed) {
        // 换源：清空旧源缓存、丢弃旧源在途响应，并立刻取新源。
        cancel_active_job();
        ++source_generation_;
        url_ = command.url;
        configured_ = true;
        health_.set_configured(true);
        scheduler_.reset(monotonic_ms());
        log_format(L"apply: 地址已切换为 %s（source generation %llu）", redact(url_).c_str(),
                   source_generation_);
    } else {
        url_ = command.url;
        if (!configured_) {
            configured_ = true;
            health_.set_configured(true);
            scheduler_.reset(monotonic_ms());
        }
        log_format(L"apply: 地址未变（%s），按命令要求执行了重绑（binding generation %llu）",
                   redact(url_).c_str(), binding_generation_);
    }
    if (command.has_pid) {
        log_format(L"apply: 绑定生产者 PID %lu", pid);
    } else {
        log_line(L"apply: 已解除生产者绑定，转为独立运行");
    }
    return ControlReply::Accepted;
}

Master::Job* Master::find_job(std::uint64_t id) {
    for (Job& job : jobs_) {
        if (job.id == id) {
            return &job;
        }
    }
    return nullptr;
}

void Master::start_job() {
    // 先把这一次取数需要的元数据全部复制成局部量，再交给线程：
    // 线程一跑起来就会拿 context，并在结束时删掉它，之后再读 context 就是 use-after-free。
    std::uint64_t const job_id = next_job_id_++;
    std::uint64_t const generation = source_generation_;
    std::wstring const url = url_;
    HttpLimits const limits{kMaxBodyBytes, kMaxRedirects, schedule_.timeout_ms};
    std::shared_ptr<std::atomic<bool>> abort = std::make_shared<std::atomic<bool>>(false);

    auto* context = new FetchJobContext();
    context->job_id = job_id;
    context->generation = generation;
    context->url = url;
    context->limits = limits;
    context->abort = abort;
    context->notify = server_.window();

    HANDLE const thread = ::CreateThread(nullptr, 0, &fetch_thread_entry, context, 0, nullptr);
    if (thread == nullptr) {
        log_format(L"fetch: CreateThread 失败：%s", last_error_text().c_str());
        delete context;
        health_.apply_failure(FailureKind::Internal, "无法启动取数线程", wall_ms());
        scheduler_.on_result(false, monotonic_ms());
        return;
    }
    Job job;
    job.id = job_id;
    job.generation = generation;
    job.thread = thread;
    job.active = true;
    job.abort = abort;
    jobs_.push_back(std::move(job));
    active_job_id_ = job_id;
    fetch_deadline_ms_ = monotonic_ms() + schedule_.timeout_ms;
    log_format(L"fetch: 开始第 %llu 次取数（job %llu，预算 %lld ms，地址 %s）", generation, job_id,
               schedule_.timeout_ms, redact(url_).c_str());
}

void Master::cancel_active_job() {
    if (active_job_id_ == 0) {
        return;
    }
    if (Job* job = find_job(active_job_id_)) {
        job->active = false;
        job->timed_out = true;
        if (job->abort) {
            job->abort->store(true);
        }
    }
    active_job_id_ = 0;
}

void Master::reap_jobs() {
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        // 只回收“结果已经处理过或已经放弃”的作业。活动作业即使线程已经结束，
        // 也要等它那条结果消息被处理完（那时 active 才会变 false），否则会把有效结果误丢。
        if (!it->active && ::WaitForSingleObject(it->thread, 0) == WAIT_OBJECT_0) {
            ::CloseHandle(it->thread);
            it = jobs_.erase(it);
        } else {
            ++it;
        }
    }
}

void Master::on_fetch_done(std::unique_ptr<FetchOutcome> outcome) {
    if (!outcome) {
        return;
    }
    Job* job = find_job(outcome->job_id);
    bool const relevant = job != nullptr && job->active && !job->timed_out &&
                          outcome->job_id == active_job_id_ &&
                          outcome->generation == source_generation_ &&
                          outcome->http.failure != HttpFailure::Aborted;
    if (!relevant) {
        log_format(L"fetch: 丢弃迟到结果（job %llu，source generation %llu，当前 %llu）",
                   outcome->job_id, outcome->generation, source_generation_);
        return;
    }
    job->active = false;
    active_job_id_ = 0;
    long long const now = monotonic_ms();
    long long const wall = wall_ms();

    if (outcome->http.ok && outcome->validation.ok) {
        health_.apply_snapshot(outcome->snapshot, wall);
        scheduler_.on_result(true, now);
        log_format(L"fetch: 成功，%zu 条%s", outcome->snapshot.items.size(),
                   outcome->snapshot.has_expires_at ? L"，带到期时间" : L"");
        return;
    }

    FailureKind kind = FailureKind::Transport;
    std::string reason;
    if (!outcome->http.ok) {
        kind = failure_from_http(outcome->http.failure);
        reason = outcome->http.error;
        if (kind == FailureKind::None) {
            return;  // 主动取消，不当作失败
        }
    } else {
        kind = failure_from_validation(outcome->validation.kind);
        reason = outcome->validation.message;
    }
    health_.apply_failure(kind, reason, wall);
    scheduler_.on_result(false, now);
    log_format(L"fetch: 失败（%s）：%s", wide_from_utf8(failure_kind_name(kind)).c_str(),
               wide_from_utf8(reason).c_str());
}

void Master::on_binding_exit(std::uint64_t generation) {
    if (generation != binding_generation_ || !binding_.active()) {
        log_format(L"binding: 忽略旧代次的退出回调（generation %llu，当前 %llu）", generation,
                   binding_generation_);
        return;
    }
    // 代次对得上还不够：当前绑定的进程句柄必须真的已经触发。一次没提交的绑定尝试留下的
    // 迟到消息会带着一个“看起来对得上”的代次到达，只按代次判定会误退新实例。
    if (!binding_.signaled()) {
        log_format(L"binding: 代次 %llu 的回调到达，但当前绑定的 PID %lu 仍在运行；忽略",
                   generation, binding_.pid());
        return;
    }
    log_format(L"binding: 绑定生产者 PID %lu 已退出；实例准备正常退出", binding_.pid());
    ::PostMessageW(server_.window(), kMsgQuit, 0, 0);
}

void Master::shutdown() {
    log_line(L"deskfeed: shutting down");
    // 取消是真的：HTTP 层每隔约 50 ms 看一次取消标志，并会关掉在途的请求句柄，
    // 所以这里的等待通常几毫秒就结束。上限只是兜底：超时也只记日志，不阻塞退出。
    for (Job& job : jobs_) {
        if (job.abort) {
            job.abort->store(true);
        }
    }
    for (Job& job : jobs_) {
        if (job.thread != nullptr) {
            if (::WaitForSingleObject(job.thread, kShutdownJoinMs) != WAIT_OBJECT_0) {
                log_format(L"deskfeed: 取数线程 %llu 未在 %u ms 内退出，放弃等待", job.id,
                           kShutdownJoinMs);
            }
            ::CloseHandle(job.thread);
            job.thread = nullptr;
        }
    }
    jobs_.clear();
    binding_.clear();
    // 正常收尾：保留最后一帧内容并置 closed，叫醒读端去清理任务栏组件。放在关控制窗口之前，
    // 让读端尽早知道主进程要走了。
    if (frame_producer_.created()) {
        frame_producer_.publish_closed(wall_ms());
    }
    server_.destroy();
}

int Master::run() {
    if (!initialize()) {
        return 7;
    }

    // 先读磁盘上的调度配置：即使启动命令一个调度开关都不给，也要沿用保存的值；
    // 命令里显式给的 --poll-ms / --retry-ms / --timeout-ms 随后覆盖它。
    SourceSettings source = settings_ ? settings_->read_source() : SourceSettings{};
    ScheduleConfig config;
    config.poll_ms = schedule_clamp_poll(source.poll_ms).value;
    config.retry_ms = schedule_clamp_retry(source.retry_ms).value;
    config.timeout_ms = schedule_clamp_timeout(source.timeout_ms).value;
    schedule_ = config;
    scheduler_.set_config(schedule_);

    if (command_.kind == CommandKind::Apply) {
        ControlReply const reply = apply_command(command_, 0);
        if (reply != ControlReply::Accepted) {
            log_line(L"deskfeed: 首次启动的命令被拒绝，退出");
            server_.destroy();
            log_close();
            return reply == ControlReply::Rejected ? 6 : 2;
        }
    } else {
        if (!source.url.empty()) {
            UrlParts parts;
            std::wstring url_error;
            if (url_crack(source.url, parts, url_error)) {
                url_ = source.url;
                configured_ = true;
                health_.set_configured(true);
                scheduler_.reset(monotonic_ms());
                log_format(L"deskfeed: 用保存的地址独立启动：%s", redact(url_).c_str());
            } else {
                configured_ = false;
                health_.set_configured(false);
                log_line(L"deskfeed: 保存的地址不合法：" + url_error);
            }
        } else {
            configured_ = false;
            health_.set_configured(false);
            log_line(L"deskfeed: 没有保存的地址；显示“未配置 URL”");
        }
    }

    // 启动时先发布一帧（force）：读端一挂上就该有东西可读，不必等第一次取数回来。
    // 这也是“任务栏组件被打开/重新注入时立刻给一帧缓存”的同一条路径。
    publish_display_frame(true);

    for (;;) {
        if (quit_requested_) {
            break;
        }
        long long const now = monotonic_ms();
        health_.tick(wall_ms());

        // 整次取数的墙钟预算由主进程守：到点就当场宣布失败并放弃这次取数，
        // 不等底层调用自己超时（慢速滴流也拖不过这个截止）。
        if (active_job_id_ != 0 && now >= fetch_deadline_ms_) {
            if (Job* job = find_job(active_job_id_)) {
                job->active = false;
                job->timed_out = true;
                if (job->abort) {
                    job->abort->store(true);
                }
            }
            active_job_id_ = 0;
            health_.apply_failure(FailureKind::Timeout,
                                  "超过 " + std::to_string(schedule_.timeout_ms) + " ms 整次取数预算",
                                  wall_ms());
            scheduler_.on_result(false, now);
            log_format(L"fetch: 超过 %lld ms 墙钟预算，放弃这一次", schedule_.timeout_ms);
        }

        // 在阻塞等待之前发布：本轮顶部的状态转换（到期 tick 与整次取数的墙钟截止）已经把
        // 显示模型改掉了，而下面的 MsgWaitForMultipleObjects 可能要睡到下一个轮询点。只在循环
        // 末尾发布会让显示面在“模型已过期”之后继续显示正常色，直到下一次取数或别的消息把它
        // 叫醒——到期由主进程按绝对时间独立判定，这一刻起就必须是读端看到的那一刻。
        // revision 没变时这里是空操作，不会多写一帧。
        publish_display_frame(false);

        std::size_t live_jobs = 0;
        for (Job const& job : jobs_) {
            if (job.active) {
                ++live_jobs;
            }
        }

        if (configured_ && active_job_id_ == 0 && scheduler_.due_ms() <= now &&
            live_jobs < kMaxLiveFetches && jobs_.size() < kMaxFetchJobs) {
            start_job();
        }

        // 等待时间：时钟域不能混。调度与超时用单调时钟，到期是快照里的绝对 Unix 时间。
        bool const at_valve = jobs_.size() >= kMaxFetchJobs;
        long long wait = 60000;
        if (active_job_id_ != 0) {
            wait = fetch_deadline_ms_ - now;
        } else if (at_valve) {
            wait = 1000;  // 作业记录已到硬上限：等某个被放弃的取数收手
        } else if (configured_) {
            wait = scheduler_.due_ms() - now;
        }
        long long const expiry = health_.next_expiry_ms();
        if (expiry >= 0) {
            long long const expiry_wait = expiry - wall_ms();
            if (expiry_wait < wait) {
                wait = expiry_wait;
            }
        }
        if (wait < 0) {
            wait = at_valve ? 1000 : 0;
        }
        if (wait > 60000) {
            wait = 60000;
        }

        HANDLE handles[kMaxFetchJobs]{};
        DWORD count = 0;
        for (Job const& job : jobs_) {
            if (count < kMaxFetchJobs) {
                handles[count++] = job.thread;
            }
        }
        DWORD const waited = ::MsgWaitForMultipleObjects(count, count > 0 ? handles : nullptr, FALSE,
                                                         static_cast<DWORD>(wait), QS_ALLINPUT);
        if (waited == WAIT_FAILED) {
            log_format(L"deskfeed: MsgWaitForMultipleObjects 失败：%s", last_error_text().c_str());
            break;
        }

        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                quit_requested_ = true;
                break;
            }
            if (message.message == kMsgFetchDone) {
                on_fetch_done(std::unique_ptr<FetchOutcome>(
                    reinterpret_cast<FetchOutcome*>(message.lParam)));
                continue;
            }
            if (message.message == kMsgBindingExit) {
                on_binding_exit(static_cast<std::uint64_t>(message.wParam));
                continue;
            }
            if (message.message == kMsgQuit) {
                quit_requested_ = true;
                continue;
            }
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }

        // 消息先处理完再回收线程：取数结果正是通过消息投递的，先回收会把结果误判成迟到。
        reap_jobs();

        // 一次循环统一发布：到期 tick、截止失败、取数结果、命令换源都在这一轮里改过模型。
        // revision 只在模型真的变了时才变，所以空转的循环不会白写一帧。
        // 顶部那次（阻塞等待之前）保证“模型变了就立刻可见”；这一次保证消息处理阶段
        // （取数结果、控制命令）引起的变化同样在本轮内落地。
        publish_display_frame(false);
    }

    shutdown();
    log_line(L"deskfeed: gone");
    log_close();
    return 0;
}

std::string Master::inspect_json() const {
    bool url_removed = false;
    std::wstring const runtime_redacted = url_redact(url_, &url_removed);
    bool disk_removed = false;
    std::wstring disk_url;
    if (settings_) {
        disk_url = settings_->read_source().url;
    }
    std::wstring const disk_redacted = url_redact(disk_url, &disk_removed);

    View const view = health_.view();
    long long const now = monotonic_ms();
    long long const next_fetch =
        configured_ ? std::max<long long>(0, scheduler_.due_ms() - now) : 0;

    std::string out = "{";
    out += "\"instance\":" + json_wstring(scope_);
    out += ",\"masterPid\":" + std::to_string(::GetCurrentProcessId());
    out += ",\"runtimeUrl\":" + json_wstring(runtime_redacted);
    out += ",\"runtimeUrlRedacted\":" + std::string(url_removed ? "true" : "false");
    out += ",\"diskUrl\":" + json_wstring(disk_redacted);
    out += ",\"configured\":" + std::string(configured_ ? "true" : "false");
    if (binding_.active()) {
        out += ",\"boundPid\":" + std::to_string(binding_.pid());
    } else {
        out += ",\"boundPid\":null";
    }
    out += ",\"sourceGeneration\":" + std::to_string(source_generation_);
    out += ",\"bindingGeneration\":" + std::to_string(binding_generation_);
    out += ",\"schedule\":{\"pollMs\":" + std::to_string(schedule_.poll_ms) +
           ",\"retryMs\":" + std::to_string(schedule_.retry_ms) +
           ",\"timeoutMs\":" + std::to_string(schedule_.timeout_ms) + "}";
    out += ",\"nextFetchInMs\":" + std::to_string(next_fetch);

    char const* kind = "waiting";
    if (view.kind == ViewKind::Content) {
        kind = "content";
    } else if (view.kind == ViewKind::Error) {
        kind = "error";
    }
    out += ",\"view\":{\"kind\":\"";
    out += kind;
    out += "\",\"alarm\":" + std::string(view.alarm ? "true" : "false");
    out += ",\"failure\":" + json_string(failure_kind_name(view.failure));
    out += ",\"reason\":" + json_string(view.reason);
    out += ",\"items\":[";
    for (std::size_t index = 0; index < view.items.size(); ++index) {
        ViewItem const& item = view.items[index];
        if (index > 0) {
            out += ",";
        }
        out += "{\"id\":" + json_string(item.id) + ",\"label\":" + json_string(item.label) +
               ",\"value\":" + json_string(item.value) +
               ",\"state\":" + json_string(item_state_name(item.state)) +
               ",\"detail\":" + json_string(item.detail) + "}";
    }
    out += "]}";

    out += ",\"hasSnapshot\":" + std::string(health_.has_snapshot() ? "true" : "false");
    out += ",\"expired\":" + std::string(health_.expired() ? "true" : "false");
    out += ",\"receivedAtMs\":";
    out += health_.has_snapshot() ? std::to_string(health_.received_at_ms()) : std::string("null");
    out += ",\"expiresAtMs\":";
    if (health_.has_snapshot() && health_.snapshot().has_expires_at) {
        out += std::to_string(health_.snapshot().expires_at_ms);
    } else {
        out += "null";
    }
    out += "}";
    return out;
}

std::wstring default_class_name(std::wstring const& scope) {
    return scoped_name(kControlWindowClassBase, scope);
}

std::wstring master_mutex_name(std::wstring const& scope) {
    return scoped_name(L"Local\\DeskFeed-master", scope);
}

int forward_command(std::vector<std::wstring> const& args, std::wstring const& class_name) {
    ControlRequest request;
    request.op = ControlOp::Command;
    request.sender_pid = ::GetCurrentProcessId();
    request.args = args;

    ControlReply reply = ControlReply::BadRequest;
    std::string error;
    if (!control_send(class_name, request, kControlSendTimeoutMs, reply, error)) {
        console_write_stderr(L"deskfeed: 没有收到实例确认（" + wide_from_utf8(error) +
                             L"）；这不代表命令没有执行\n");
        return 5;
    }
    switch (reply) {
        case ControlReply::Accepted:
        case ControlReply::NotBound:
            return 0;
        case ControlReply::Rejected:
        case ControlReply::BadRequest:
            console_write_stderr(L"deskfeed: 运行中的实例拒绝了这条命令\n");
            return 6;
    }
    return 6;
}

int inspect_instance(std::wstring const& class_name, std::wstring const& scope) {
    unsigned long const self = ::GetCurrentProcessId();
    ControlRequest request;
    request.op = ControlOp::Inspect;
    request.sender_pid = self;

    ControlReply reply = ControlReply::BadRequest;
    std::string error;
    if (!control_send(class_name, request, kControlSendTimeoutMs, reply, error)) {
        console_write_stderr(L"deskfeed: 没有运行中的实例（" + wide_from_utf8(error) + L"）\n");
        return 5;
    }
    if (reply != ControlReply::Accepted) {
        console_write_stderr(L"deskfeed: 实例拒绝了查询\n");
        return 6;
    }

    std::wstring const path = deskfeed_inspect_path(scope, self);
    std::string bytes;
    bool exists = false;
    std::wstring read_error;
    if (!read_file_bytes(path, bytes, exists, read_error) || !exists) {
        console_write_stderr(L"deskfeed: 读不到查询结果 " + path + L"\n");
        return 7;
    }
    console_write_stdout(bytes);
    console_write_stdout("\n");
    delete_file(path);
    return 0;
}

}  // namespace

int master_main(std::vector<std::wstring> const& args) {
    Command command;
    std::wstring parse_error;
    if (!command_parse(args, command, parse_error)) {
        console_write_stderr(L"deskfeed: " + parse_error + L"\n\n" +
                             wide_from_utf8(command_usage()));
        return 2;
    }
    if (command.kind == CommandKind::Help) {
        console_write_stdout(command_usage());
        return 0;
    }

    // 实例命名空间：显式 --scope 优先，其次环境变量；两者都要过白名单。
    std::wstring const scope = command.scope.empty() ? instance_suffix() : command.scope;
    std::wstring scope_error;
    if (!scope_validate(scope, scope_error)) {
        console_write_stderr(L"deskfeed: " + scope_error + L"\n");
        return 2;
    }

    std::wstring const class_name = default_class_name(scope);
    std::wstring const mutex_name = master_mutex_name(scope);

    UserSecurity security;
    if (!security.build()) {
        // 建不出只允许当前用户的安全描述符就退回系统默认；对象名仍带用户哈希。
        console_write_stderr(L"deskfeed: 警告：没能建立用户范围的安全描述符，改用系统默认\n");
    }

    InstanceMutex mutex;
    bool already_running = false;
    std::string mutex_error;
    if (!mutex.acquire(mutex_name, &security, already_running, mutex_error)) {
        console_write_stderr(L"deskfeed: " + wide_from_utf8(mutex_error) + L"\n");
        return 7;
    }

    switch (command.kind) {
        case CommandKind::QuitIfBound:
        case CommandKind::Quit:
            if (!already_running) {
                return 0;  // 条件退出没有实例：成功返回，不启动任何东西
            }
            return forward_command(args, class_name);
        case CommandKind::Inspect:
            if (!already_running) {
                console_write_stderr(L"deskfeed: 没有运行中的实例\n");
                return 5;
            }
            return inspect_instance(class_name, scope);
        case CommandKind::Apply:
            if (already_running) {
                return forward_command(args, class_name);
            }
            break;
        case CommandKind::Start:
            if (already_running) {
                return 0;  // 无参数：不接管，也不改变地址与绑定
            }
            break;
        case CommandKind::Help:
            return 0;
    }

    Master master(command, scope);
    return master.run();
}

}  // namespace deskfeed
