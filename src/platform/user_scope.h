#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

// 当前用户范围的安全描述符与进程归属检查。
//
// 命名对象（互斥体、共享内存、事件）都带当前用户 SID 的短哈希后缀，避免同一登录会话里
// 另一个用户的同名实例被误认成“自己的单例”；再叠一层只允许当前用户访问的 DACL，
// 让别的用户即使猜到名字也打不开。控制通道还会核对发送方的进程属主与自身一致，
// 见 docs/lifecycle.md 的“同一用户范围”。
namespace deskfeed {

// 只允许当前用户访问的 SECURITY_ATTRIBUTES。build() 失败时 attributes() 返回 nullptr，
// 调用方退回系统默认安全描述符（此时对象仍然按用户后缀命名）。
class UserSecurity {
public:
    bool build();
    bool ready() const { return ready_; }
    SECURITY_ATTRIBUTES const* attributes() const { return ready_ ? &attributes_ : nullptr; }

private:
    std::vector<unsigned char> descriptor_;
    SECURITY_ATTRIBUTES attributes_{};
    bool ready_ = false;
};

// 指定进程的属主 SID 是否与当前进程一致。打不开对方进程（权限或已退出）时返回 false。
bool process_user_matches(std::uint32_t process_id);

}  // namespace deskfeed
