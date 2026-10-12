#include "display/reading.h"

#include <algorithm>
#include <string>
#include <vector>

// 纯文本排版（见 reading.h）。这个文件刻意不 include 任何 Windows / JSON 头：
// 它既要被桌面悬浮窗与任务栏负载链接，也要被离线量具单独编译，所以只依赖 frame.h 的模型。
namespace deskfeed {
namespace {

constexpr char const* kNoRowsText = "（没有可显示的来源：窗口会整窗隐藏）";
constexpr char const* kEmptyDetailText = "（空）";
constexpr char const* kEmptyNotesText = "（无）";

// 把一段多行文本按 '\n' 拆开（末尾的空行保留为空串，与按行遍历一致）。
std::vector<std::string> split_lines(std::string const& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (;;) {
        std::size_t const at = text.find('\n', start);
        if (at == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, at - start));
        start = at + 1;
    }
    return lines;
}

// 悬停文本里不能有裸换行：`--reading` 的 tip 是单行，换行换成 " / "（旧量具的同一取舍）。
std::string tip_on_one_line(std::string const& tip) {
    std::string out;
    out.reserve(tip.size());
    for (char const ch : tip) {
        if (ch == '\n') {
            out += " / ";
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

// 整体失效时这一行要写的文本：原因在前，生产端 detail 跟在后面（有才跟）。
// 这样整体失效的原因看得见，生产端原本写的那点上下文也不会被顶掉。
std::string failure_text_with_detail(std::string const& reason, std::string const& detail) {
    if (detail.empty()) {
        return reason;
    }
    if (reason.empty()) {
        return detail;
    }
    return reason + "\n" + detail;
}

// 被有界分组折掉的条目，在标记的悬停文本里怎么出现：行文本（label + value）+ 状态。
// 提示里没有颜色，状态只能用文字表示；只有非正常档才加标记，正常档不添噪音。
std::string hidden_row_text(ReadingRow const& row) {
    std::string text = reading_row_text(row);
    if (row.tone == ReadingTone::Warn) {
        text += "（警告）";
    } else if (row.tone == ReadingTone::Error) {
        text += "（错误）";
    }
    return text;
}

}  // namespace

ReadingTone reading_tone_of(ItemState state) {
    switch (state) {
        case ItemState::Ok:
            return ReadingTone::Normal;
        case ItemState::Warn:
            return ReadingTone::Warn;
        case ItemState::Error:
            return ReadingTone::Error;
    }
    return ReadingTone::Normal;
}

ReadingContent reading_from_frame(Frame const& frame) {
    ReadingContent content;
    content.reason = frame.reason;

    if (frame.kind == ViewKind::Waiting) {
        // 首次请求等待中：一行都不给。窗口据此不出现，也不会先露一个空窗。
        return content;
    }

    if (frame.kind == ViewKind::Error) {
        // 没有可显示的值：唯一一行红色 ERROR，原因写进 tip 与展开详情。
        ReadingRow row;
        row.value = "ERROR";
        row.tone = ReadingTone::Error;
        row.tip = frame.reason;
        row.detail = frame.reason;
        content.rows.push_back(std::move(row));
        content.failure = true;
        return content;
    }

    // ViewKind::Content：每条目一行。整体失效（alarm）时保留值、整行标红，
    // 并把原因并进 tip/detail，而不是把生产端的原文换成原因。
    for (FrameItem const& item : frame.items) {
        ReadingRow row;
        row.label = item.label;
        row.value = item.value;
        row.state = item.state;
        row.tone = frame.alarm ? ReadingTone::Error : reading_tone_of(item.state);
        row.tip = frame.alarm ? failure_text_with_detail(frame.reason, item.detail) : item.detail;
        row.detail = row.tip;
        content.rows.push_back(std::move(row));
    }
    return content;
}

std::size_t reading_per_line_for(std::size_t rows) {
    if (rows <= kMaxReadingLines) {
        return 1;
    }
    return (rows + kMaxReadingLines - 1) / kMaxReadingLines;
}

std::vector<ReadingLine> reading_pack(ReadingContent const& content, std::size_t per_line) {
    std::vector<ReadingLine> lines;
    if (content.rows.empty()) {
        return lines;
    }
    std::size_t const width = per_line == 0 ? reading_per_line_for(content.rows.size()) : per_line;
    std::size_t index = 0;
    while (index < content.rows.size()) {
        std::size_t const group_end = std::min(content.rows.size(), index + width);
        ReadingLine line;
        std::size_t const shown_end = std::min(group_end, index + kMaxSegmentsPerLine);
        for (std::size_t at = index; at < shown_end; ++at) {
            line.segments.push_back(content.rows[at]);
        }
        line.hidden = group_end - shown_end;
        if (line.hidden != 0) {
            // 有界分组的标记本身就是这一段：它没有 label，画法与量具都按普通段走。
            // 它的悬停文本逐条列出被折掉的条目——只画一个「另有 N 项」而说不出是哪些，
            // 折掉的 value 就再也看不到了。
            ReadingRow marker;
            marker.value = "…（另有 " + std::to_string(line.hidden) + " 项）";
            for (std::size_t at = shown_end; at < group_end; ++at) {
                if (!marker.tip.empty()) {
                    marker.tip += '\n';
                }
                marker.tip += hidden_row_text(content.rows[at]);
            }
            line.segments.push_back(std::move(marker));
        }
        lines.push_back(std::move(line));
        index = group_end;
    }
    return lines;
}

std::string reading_row_text(ReadingRow const& row) {
    if (row.label.empty()) {
        return row.value;
    }
    // 前缀与读数之间固定一个空格（与旧挂件“前缀 读数”的口径一致）。
    return row.label + " " + row.value;
}

std::string reading_line_text(ReadingLine const& line) {
    std::string text;
    for (ReadingRow const& segment : line.segments) {
        if (!text.empty()) {
            text += " · ";
        }
        text += reading_row_text(segment);
    }
    return text;
}

std::string reading_line_tip(ReadingLine const& line) {
    // 非空的 tip 之间用 '\n' 连（空的不产生分隔符，与旧实现一致——否则空 tip 会在提示里
    // 留下一条多余的空行）。
    std::string joined;
    for (ReadingRow const& segment : line.segments) {
        if (segment.tip.empty()) {
            continue;
        }
        if (!joined.empty()) {
            joined += '\n';
        }
        joined += segment.tip;
    }
    return joined;
}

std::string reading_detail_text(ReadingContent const& content) {
    std::string text;
    for (ReadingRow const& row : content.rows) {
        // 标题就是这一条的行文本：value 一定在里面，不依赖生产端写 detail。
        // 只列 detail 非空的条目会让“有 value、没 detail”的条目（协议允许）整个消失，
        // 有界分组折掉的那些值就再也找不回来了。
        text += reading_row_text(row);
        text += '\n';
        if (row.detail.empty()) {
            // 没有 detail 就不留那一行缩进：split_lines("") 会给出一条空行，
            // 照抄会写出一行只有两个空格的空行。
            continue;
        }
        for (std::string const& line : split_lines(row.detail)) {
            text += "  ";
            text += line;
            text += '\n';
        }
    }
    // 上面每条都补了行尾换行，最后再削掉一个：段落之间不留空行，末尾也不多一个空行。
    if (!text.empty()) {
        text.pop_back();
    }
    return text;
}

std::string reading_rows_text(ReadingContent const& content, long long poll_ms) {
    std::string text;
    text += "reading: rows=" + std::to_string(content.rows.size());
    // v1 没有“沿用上一次读数”的宽限期：失败即整体失效，所以这里恒为 no。
    text += " stale=no failed=";
    text += content.failure ? "yes" : "no";
    text += " refresh_ms=" + std::to_string(poll_ms);
    text += '\n';

    for (std::size_t index = 0; index < content.rows.size(); ++index) {
        ReadingRow const& row = content.rows[index];
        text += "row " + std::to_string(index) + ": error=";
        text += row.tone == ReadingTone::Error ? "1" : "0";
        text += " prefix=<" + row.label + "> value=<" + row.value + ">\n";
    }

    std::size_t const auto_per_line = reading_per_line_for(content.rows.size());
    std::vector<ReadingLine> const auto_lines = reading_pack(content, auto_per_line);
    text += "lines: auto (per_line=" + std::to_string(auto_per_line) +
            ", max " + std::to_string(kMaxReadingLines) + ") -> " +
            std::to_string(auto_lines.size()) + " line(s)\n";
    for (std::size_t index = 0; index < auto_lines.size(); ++index) {
        text += "  line " + std::to_string(index) + ": " + reading_line_text(auto_lines[index]) + "\n";
    }

    // 显式每行条目数的覆盖值：列 1..5，让配置里的覆盖值始终可核对。
    for (std::size_t per_line = 1; per_line <= 5; ++per_line) {
        text += "lines: sources_per_line=" + std::to_string(per_line) + " -> " +
                std::to_string(reading_pack(content, per_line).size()) + " line(s)\n";
    }
    return text;
}

std::string reading_dump_text(ReadingContent const& content) {
    std::string text;
    std::size_t const auto_per_line = reading_per_line_for(content.rows.size());
    for (ReadingLine const& line : reading_pack(content, auto_per_line)) {
        text += reading_line_text(line);
        text += "     [";
        text += tip_on_one_line(reading_line_tip(line));
        text += "]\n";
    }
    if (content.rows.empty()) {
        text += kNoRowsText;
        text += '\n';
    }

    text += '\n';
    text += "--- 展开 ---\n";
    std::string const detail = reading_detail_text(content);
    text += detail.empty() ? kEmptyDetailText : detail;
    text += '\n';
    text += "--- 未显示的来源 ---\n";
    // v1 的帧里没有“未显示的条目”这个概念（没出现的条目就是没出现），所以这一段恒为空。
    text += kEmptyNotesText;
    text += '\n';
    return text;
}

}  // namespace deskfeed
