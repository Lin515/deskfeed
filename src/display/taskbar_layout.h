#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// 托盘在哪些屏幕上显示：配置文本与“当前可用目标”的纯逻辑。
//
// 这一层不碰 Windows、不碰 XAML、不读文件，所以配置解析与退化规则可以在离线用例里逐条咬。
// 屏幕身份用显示器接口路径（`\\?\DISPLAY#...`）而不是 `\\.\DISPLAY2` 这种按当前拓扑排出来的
// 名字：后者在拔插、换主屏、改分辨率之后会整体移位，配置就会指到另一块屏幕上。
namespace deskfeed {

// 一份配置最多描述多少块屏幕。上限只是边界，不是产品承诺：普通桌面远用不到。
inline constexpr std::size_t kMaxTaskbarScreens = 16;

// 单个屏幕标识的字符数上限。显示器接口路径是 ASCII，实际长度约 100 字符。
inline constexpr std::size_t kMaxTaskbarScreenIdChars = 512;

// 标识是否可安全写进 `[taskbar] screens`：非空、有界、可打印、不含分隔符 `;` 与换行。
bool taskbar_screen_id_valid(std::wstring_view id);

// 大小写不敏感的标识比较。Windows 的显示器接口路径大小写可能被驱动改写，配置与运行时
// 必须按同一块屏幕处理。
bool taskbar_screen_ids_equal(std::wstring_view left, std::wstring_view right);

// 解析 `id;id;...`。空段被忽略，重复项按第一次出现保留，非法段被丢弃并置 malformed。
// `text` 只含空白时得到空表且 malformed=false（显式关闭“在任意任务栏显示”）。
std::vector<std::wstring> parse_taskbar_screen_list(std::wstring_view text,
                                                    bool* malformed = nullptr);

// 格式化成 `id;id;...`。任何非法、重复或超过上限的输入都返回 false，out 不被使用。
bool format_taskbar_screen_list(std::vector<std::wstring> const& ids, std::wstring& out);

struct TaskbarScreenSelectionInput {
    // 设置文件里有没有 `[taskbar] screens`。没有时按“只在主屏显示”这一向后兼容的缺省处理。
    bool configured = false;
    // configured 为真时的**启用集合**；可以是空表，表示明确不在任何任务栏显示。
    std::vector<std::wstring> enabled;
    // 当前主屏的标识。拿不到时为空。
    std::wstring primary;
    // 当前真正能落块的屏幕（任务栏窗口存在且托盘元素已经解析出来）。
    std::vector<std::wstring> available;
};

struct TaskbarScreenSelection {
    // 本次真正要显示在哪些屏幕上，按 available 的顺序输出。
    std::vector<std::wstring> selected;
    // 退化发生了：配置里启用了屏幕，但这些屏幕一个都不可用，于是只在主屏显示。
    // **退化不改变配置**；菜单的勾仍然按配置画。
    bool degraded = false;
    // 配置（或缺省）确实要求至少显示一块屏幕。用于把“明确关闭”与“启用了但全不可用”分开。
    bool requested_any = false;
};

// 选择规则（docs/taskbar-injection.md 的“多屏与退化”一节是面向用户的同一份说明）：
//   * 配置了哪些屏幕就只在这些屏幕里选，交集为空且原集合非空时才退化到主屏；
//   * 配置为空（明确关闭）不退化；
//   * 配置了多块而只有部分不可用，不退化——可用的那几块照常显示；
//   * 主屏自己也不可用时没有可退化的目标，selected 为空。
TaskbarScreenSelection select_taskbar_screens(TaskbarScreenSelectionInput const& input);

}  // namespace deskfeed
