#pragma once

#include <windows.h>

#include <string>

#include "core/utf8.h"
#include "display/mailbox.h"
#include "platform/paths.h"
#include "platform/win32_util.h"

// 自测宿主的 **scope 仲裁**：与生产主进程抢**同一个**名字
// （`paths.h::master_mutex_name(scope)` = `scoped_name(L"Local\\DeskFeed-master", scope)`）。
//
// 为什么必须是这个名字，而不是探针私有的一个名字：
//   * 只让探针之间互斥挡不住真实主进程。检查"邮箱还不存在"之后、真正 `create` 之前，一个主进程
//     可能刚好把这台实例建起来；探针随后的 `DisplayFrameProducer::create` 会把它的帧头 memset 掉
//     （`display/mailbox.cpp` 的 create 对已存在的同名 section 照样成功）。抢同一个仲裁名字之后，
//     主进程的实例锁会拿到 `ERROR_ALREADY_EXISTS`，于是它 fail closed（不建状态、不取数、不注入），
//     这条 TOCTOU 窗口才真的关上。
//   * 命名对象"存在即被占"是双方看到的同一个事实：主进程的 `InstanceMutex` 与这里一样是
//     `CreateMutex` + `ERROR_ALREADY_EXISTS`，不等所有权。
//   * 探针绝不碰生产命名空间：空 scope 在平台层表示生产实例，`probe_scope_ok`（probe/host.h）
//     直接拒绝，所以这里永远不会与生产实例抢名字；scope 白名单还把名字限死在 1..32 个
//     `[A-Za-z0-9_-]`。
//
// 持有期：**本次运行的全部 probe producer**（含 `ownerdeath` 的辅助子进程）都结束之后才归还。
// claim 是内核对象，进程死亡时句柄自动关闭 ⇒ 崩掉的探针不会留下永久占用。
//
// 次序是刻意的：**先抢仲裁名字、再看邮箱在不在**。赢下名字之后，别的 master/探针不可能正在新建
// 这块邮箱，所以那次"存在性检查"的结论是稳定的；反过来先查再抢会留下"查完空、别人建好、我们再
// create 覆写"的窗口。
namespace deskfeed::probe {

class ProbeScopeClaim {
public:
    ProbeScopeClaim() = default;
    ~ProbeScopeClaim() { release(); }
    ProbeScopeClaim(ProbeScopeClaim const&) = delete;
    ProbeScopeClaim& operator=(ProbeScopeClaim const&) = delete;

    // 本进程去抢这个名字。已被主进程或另一个探针占着时返回 false（`error` 说明原因）。
    bool acquire(std::wstring const& scope, std::string& error) {
        return acquire_impl(scope, /*parent_holds=*/false, error);
    }

    // **父子模式**：父进程（探针）已经持有同一个 claim 贯穿整段运行，子进程不要再抢——抢就会撞上
    // 父进程自己的名字而自拒。子进程仍然做 scope 白名单校验与"邮箱已存在就拒绝"，只是把仲裁那一步
    // 交给父进程的持有期来保证。两条路径共用这一份实现，行为不会分叉成两套。
    bool acquire_with_parent_claim(std::wstring const& scope, std::string& error) {
        return acquire_impl(scope, /*parent_holds=*/true, error);
    }

    void release() {
        if (claim_ != nullptr) {
            ::CloseHandle(claim_);
            claim_ = nullptr;
        }
    }

    bool held() const { return claim_ != nullptr; }

private:
    bool acquire_impl(std::wstring const& scope, bool parent_holds, std::string& error) {
        release();
        std::wstring scope_error;
        if (scope.empty() || !deskfeed::scope_validate(scope, scope_error)) {
            error = "自测宿主需要一个合法的非空实例名";
            return false;
        }
        if (!parent_holds) {
            std::wstring const name = deskfeed::scoped_name(L"Local\\DeskFeed-master", scope);
            HANDLE const object = ::CreateMutexW(nullptr, FALSE, name.c_str());
            if (object == nullptr) {
                error = "建不了实例仲裁标记：" + deskfeed::utf8_from_wide(deskfeed::last_error_text());
                return false;
            }
            if (::GetLastError() == ERROR_ALREADY_EXISTS) {
                ::CloseHandle(object);
                error = "该实例名已被主进程或另一个探针占用；拒绝与它共用同一个邮箱";
                return false;
            }
            claim_ = object;
        }
        // 仲裁已到手（或由父进程持有）之后再读：命中说明这是**别人遗留**的 mapping。仍然拒绝——
        // 绝不去覆写一份不是本次运行建立的帧头。
        std::wstring const map_name = deskfeed::display_frame_map_name(scope);
        HANDLE const existing = ::OpenFileMappingW(FILE_MAP_READ, FALSE, map_name.c_str());
        if (existing != nullptr) {
            ::CloseHandle(existing);
            error = "该实例名已经有一块显示邮箱；拒绝覆写它的帧头";
            release();
            return false;
        }
        return true;
    }

    HANDLE claim_ = nullptr;
};

}  // namespace deskfeed::probe
