#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "display/frame.h"  // Frame / FrameItem / ViewKind / FailureKind / ItemState

// 显示格式化：把主进程已经算好的显示帧（Frame）换算成“要画的行”。
//
// 这一层是桌面悬浮窗、任务栏读数块与离线量具（`--reading` / `--reading-rows`）共用的**唯一**
// 排版口径，所以它只做文本换算：不碰 Windows、不解析 JSON、不联网、不碰共享内存，可以离线
// 逐字比对。它也不解释任何业务语义——条目文本原样使用，颜色只由 `state` / 整体失效决定。
//
// 口径（与旧原生实现逐条对齐，见 src/widget/README.md 的“显示规则”）：
//   * 逻辑行 = 帧里每个条目一行；文本是 `label + " " + value`（label 为空时只写 value）。
//   * 折行 = 每行放几个条目由 `reading_per_line_for` 现算，**最多 kMaxReadingLines 行**。
//   * 悬停文本 = 条目的 `detail`；整体失效时是“失效原因 + 条目 detail”（原因在前）。
//   * 展开详情 = 各条目 `detail` 的缩进块（见 `reading_detail_text`）。
//   * 整体失效 = 有值时保留值并全部染红，没有值时一行红色 `ERROR` 加原因。
namespace deskfeed {

// 读数最多排几行：3 个及以下一行一个，4 个及以上折行，但行数不超过这个值。
inline constexpr std::size_t kMaxReadingLines = 3;

// 一条视觉行最多放几个条目。这是 **>5 条的通用有界分组**：按 `reading_per_line_for` 折行后，
// 条目多到一行要放超过这个数时，多出来的条目不再画在这一行上，改成行尾一个
// `…（另有 N 项）` 标记；标记的悬停文本逐条列出被折掉的 `label value`（非正常状态带
// `（警告）`/`（错误）`），展开详情也逐条列出**每一个**条目的 `label value`。所以有界分组
// 只是不画在行上，不是把内容丢掉。
//
// ≤5 条时每行最多 2 个条目，永远碰不到这个上限，所以旧原生基线的逐字比对不受影响；
// 上限存在的意义是 64 条时一行不会横向长到屏幕外（64 条按公式是每行 22 个）。
inline constexpr std::size_t kMaxSegmentsPerLine = 4;

// 一行该用哪一档颜色。刻意是独立的一档枚举而不是一个 error 布尔：
// 生产端的 `state: "warn"` 必须留下自己的位置，不能被折成正常色，也不能被折成错误色。
enum class ReadingTone {
    Normal = 0,  // 正常
    Warn = 1,    // 生产端声明的警告状态
    Error = 2,   // 生产端声明的错误状态，或整体失效（整帧 alarm）
};

// 生产端的条目状态 → 颜色档。整体失效时由 `reading_from_frame` 覆盖成 Error。
ReadingTone reading_tone_of(ItemState state);

// 显示模型里的一行（逻辑行，未折行）。
struct ReadingRow {
    std::string label;  // 条目 label，原样（前缀；为空表示这一行不写前缀）
    std::string value;  // 条目 value，或整体失效时的 `ERROR`
    ReadingTone tone = ReadingTone::Normal;
    // 生产端声明的原始状态，原样保留。整体失效是**另一条信息**（帧的 alarm / tone），
    // 不会改写这里：`HealthState::view()` 逐条拷 `SnapshotItem::state`，只在 view 上单独置
    // `alarm`，所以 alarm 为真时这里仍然是生产端那一份。调用方要区分“业务状态”与
    // “整体失效”就看这个字段，不要从 tone 反推。
    ItemState state = ItemState::Ok;
    std::string tip;     // 悬停文本
    std::string detail;  // 展开详情里的原文
};

// 一条视觉行：1..kMaxSegmentsPerLine 个逻辑行。超过上限时末尾会多一个合成段
// （label 为空、value 是 `…（另有 N 项）`、tip 逐条列出被折掉的那些 `label value`），
// `hidden` 同时记下被折掉的条目数。
struct ReadingLine {
    std::vector<ReadingRow> segments;
    std::size_t hidden = 0;
};

// 一帧换算出来的全部显示内容。
struct ReadingContent {
    std::vector<ReadingRow> rows;  // 逻辑行，顺序 = 帧里的条目顺序
    bool failure = false;          // 整体失效（没有可显示的值，只有一行 ERROR）
    std::string reason;            // 整体失效的原因（状态行与悬停用）
};

// 帧 → 显示内容（不折行）。
//
//   * `ViewKind::Waiting`（首次请求等待中）给出 0 行：不显示读数，也不显示 ERROR；
//   * `ViewKind::Error`（没有可显示的值）给出一行红色 `ERROR`，tip/detail 是 reason；
//   * `ViewKind::Content` 每条目一行；`alarm` 为真时保留各条目的值但整行标成 Error，
//     tip/detail 变成“失效原因 + 生产端 detail”（原因在前，不把生产端原文顶掉）。
ReadingContent reading_from_frame(Frame const& frame);

// 按条目数算出“每行几个条目”：让行数不超过 kMaxReadingLines 的最小值。
// 3 个及以下给 1（一行一个），更多时给 ceil(rows / kMaxReadingLines)（4/5/6 个给 2，7/8/9 个给 3）。
std::size_t reading_per_line_for(std::size_t rows);

// 按每行 per_line 个条目切成视觉行；per_line 为 0 时按 `reading_per_line_for` 自动取值。
// 条目为空时返回空表。
std::vector<ReadingLine> reading_pack(ReadingContent const& content, std::size_t per_line);

// 逻辑行文本：`label + " " + value`（label 为空时只写 value）。
std::string reading_row_text(ReadingRow const& row);

// 视觉行文本：各段用 " · " 连接。
std::string reading_line_text(ReadingLine const& line);

// 视觉行的悬停文本：段内非空的 tip 用 '\n' 连接（空 tip 不产生分隔符，否则提示里会多出空行）。
// 折行后的行就是组内各条目的 tip 拼起来，有界分组的标记段也在这里带上它列出的隐藏条目。
// 量具的 `--reading` 与窗口的悬停提示都走这一个函数，两边不可能对同一条行给出不同的提示。
std::string reading_line_tip(ReadingLine const& line);

// 展开详情：**逐条列出每一个条目**，标题是该条目的行文本 `label + " " + value`
// （label 为空时只有 value）；条目自己的 detail 非空时，再把 detail 的每一行缩进两个空格
// 跟在标题下面。
//
// 这里刻意不以“有没有 detail”决定要不要出现：value 是条目唯一的主读数，生产端完全可能
// 只给 value 不给 detail（协议允许 detail 省略）。若只列 detail 非空的条目，有界分组折掉的
// 那些 value 就再也看不到了。也不要求生产端把 value 重复写进 detail——展开块自己带上它。
// 条目为空时返回空串。
std::string reading_detail_text(ReadingContent const& content);

// `--reading-rows` 的等价文本（UTF-8，行尾 '\n'）：
//
//   reading: rows=N stale=no failed=yes|no refresh_ms=<poll_ms>
//   row K: error=0|1 prefix=<label> value=<value>
//   lines: auto (per_line=P, max 3) -> K line(s)
//     line K: <按 auto 折行后的行文本>
//   lines: sources_per_line=1 -> K line(s)   （1..5 各一行）
//
// `stale` 恒为 no：v1 协议没有“沿用上一次读数”的宽限期，失败一发生就是整体失效。
// `error` 只看 `ReadingTone::Error`（warn 与 normal 都印 0），与旧量具逐字一致。
std::string reading_rows_text(ReadingContent const& content, long long poll_ms);

// `--reading` 的等价文本（UTF-8，行尾 '\n'）：折行后的行加 `     [tip]`
// （tip 里的换行换成 " / "），空内容写一行说明，然后是 `--- 展开 ---` 与
// `--- 未显示的来源 ---` 两段。这一段与 `--reading-rows` 是同一份排版的两种视图。
std::string reading_dump_text(ReadingContent const& content);

}  // namespace deskfeed
