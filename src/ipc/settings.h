#pragma once

#include <string>
#include <vector>

// settings.ini 的读写。
//
// 文件是“用户设置 + 预留的未来界面状态”的唯一存放点（位置见 platform/paths.h）。
// 两条硬约束：
//   * 事务性写入：先写同目录临时文件并刷盘，再原子替换目标文件；失败时目标文件保持原样。
//   * 不丢别的设置：只改自己负责的那一行，其它栏目、其它键、注释与顺序原样保留。
//     URL 换源只是把这一行换掉，界面状态、任务栏开关等无关设置一个字节都不动。
//
// PID、进程句柄、绑定代次、缓存内容一律不落盘（见 docs/lifecycle.md）。
namespace deskfeed {

struct SourceSettings {
    std::wstring url;
    long long poll_ms = 0;     // 0 表示没配置
    long long retry_ms = 0;
    long long timeout_ms = 0;
};

class SettingsFile {
public:
    explicit SettingsFile(std::wstring path);

    std::wstring const& path() const { return path_; }

    // 文件不存在时算成功（空设置）。解析失败（不是合法 UTF-8）返回 false。
    bool load(std::wstring& error);

    // 读取键值；键名与栏目名大小写不敏感，值去首尾空格。不存在返回 false。
    bool get(std::wstring_view section, std::wstring_view key, std::wstring& value) const;
    long long get_integer(std::wstring_view section, std::wstring_view key,
                          long long fallback) const;

    // 设置一行；changed 说明值是否真的变了（同值也走一遍写盘，便于显式提交）。
    bool set(std::wstring_view section, std::wstring_view key, std::wstring const& value,
             bool* changed, std::wstring& error);

    // 原子替换落盘。没有改动过也可以调用。
    bool save(std::wstring& error);

    // 把**内存里这份内容**原子写到另一个路径（同目录临时文件 + 替换），不回读。
    //
    // 配置事务用它：先复制一份“候选”，在候选上改，把候选落盘；成功了才把候选接进正在用的
    // 这一份。于是“保存失败”永远不会把这一份改成半吊子状态——不需要靠“再读一次盘”去回滚，
    // 也就不存在“保存失败 + 回读也失败”时留下被拒候选的那种局面。
    bool save_as(std::wstring const& path, std::wstring& error);

    // 把另一份（已经成功落盘的）内容与路径接过来。用于事务提交；不读盘。
    void adopt(SettingsFile const& committed);

    // [source] 一节的原始值（不做默认值与上下限处理，交给调用方）。
    SourceSettings read_source() const;

    // 换源事务的落盘步骤：只改 [source] url 这一行。
    bool write_source_url(std::wstring const& url, bool* changed, std::wstring& error);

private:
    struct Line {
        std::wstring raw;      // 原样（不含行尾）
        bool is_section = false;
        std::wstring section;  // is_section 时是栏目名，否则是所属栏目
        std::wstring key;      // 键值行才有
        std::wstring value;
        bool is_pair = false;
    };

    void parse();
    std::wstring render() const;

    std::wstring path_;
    std::wstring text_;  // 打开时的原文；渲染时按行原样写回
    std::vector<Line> lines_;
    bool loaded_ = false;
};

}  // namespace deskfeed
