#pragma once

#include <string>

// 从**磁盘上的 PE 文件**解析一个导出函数的 RVA，并对"能不能把它当目标进程里的一个函数入口"
// 做严格校验。
//
// 为什么不用 LoadLibrary 拿地址：注入器绝不能为了取一个地址就把负载映射进自己的地址空间。那会
// 在注入器进程里跑一遍 DllMain、留下模块与它的全局状态，也会让"负载只被装进目标进程"这条边界
// 失效。所以导出表是从文件字节里读的。
//
// 三条硬事实决定了这个函数的形状（都是踩过的坑）：
//   * 导出目录以及名字/序号/函数三张表在 PE 里给的都是 **RVA**，必须经节表换算成文件偏移；
//     直接把 RVA 当文件偏移用会落到错误的字节上，或者在"导出目录不在任何节里"上报错。
//   * **转发导出（forwarder）不能当函数地址用**：它的 RVA 落在导出目录自身范围内，那里存的是
//     `DLL.函数` 这样的字符串，不是代码。把它当远程线程入口会让目标进程在数据上执行。
//     本项目只导出自己的函数，遇到转发导出说明拿到的不是预期的那份 DLL，因此**安全拒绝**。
//   * **导出表里的 RVA 在成为线程入口之前必须再验一遍**：0、落在 image 之外、或落在不可执行的
//     节（`.rdata`/数据节）里，都会让 `CreateRemoteThread` 在目标进程里跳进 DOS 头/数据——
//     那是**打崩 explorer** 的操作，不是注入失败。所以除了解析，这里还要求：AMD64 镜像、
//     `SizeOfImage` 与 `NumberOfRvaAndSizes` 合法、函数 RVA 非 0 且落在一个带
//     `IMAGE_SCN_MEM_EXECUTE` 的节的虚拟范围里。负载只导出线程函数，没有可执行数据，
//     所以"导出的是数据"一律拒绝。
namespace deskfeed::inject {

// 解析失败的原因分类。调用方用它区分"文件打不开"与"这是一份转发导出"。
enum class PeExportError {
    None = 0,
    OpenFailed,         // 打不开 / 映射不了文件
    NotPe,              // 不是 PE 镜像（MZ / PE 签名、头长度不对）
    BadHeader,          // 可选头、节表或导出表越界
    UnsupportedMachine, // 不是 AMD64（PE32 / x86 / 其它架构都拒绝：本工程只有 x64）
    NoExportDirectory,  // 镜像没有导出目录（或数据目录项数不够）
    TablesNotMapped,    // 导出目录或三张表不落在任何一个节里
    NotFound,           // 没有这个名字的导出
    Forwarded,          // 该导出是转发导出：明确拒绝，不解析目标
    NotExecutable,      // 导出的 RVA 是 0、落在 image 之外，或不在可执行的节里
};

char const* pe_export_error_name(PeExportError kind);

struct PeExportInfo {
    unsigned long rva = 0;            // 模块内偏移（**磁盘侧候选值**）
    unsigned long long image_size = 0;  // PE 的 SizeOfImage（用来和目标模块的 modBaseSize 交叉核对）
    unsigned short machine = 0;       // COFF Machine（本工程只接受 0x8664）
    // 以下字段用于"磁盘这份 build 与远程装载的那份是不是同一个"的一致性核验（见 remote.h 的
    // `remote_image_identity` / `image_identity_matches`）：TimeDateStamp、PE32+ 标记与导出 RVA
    // 所在**可执行节**的虚拟区间都交出去，调用方不再只看 SizeOfImage。
    unsigned long time_date_stamp = 0;
    bool is_pe32_plus = false;
    unsigned long section_va = 0;        // 导出 RVA 所在节的虚拟起点
    unsigned long long section_end = 0;  // 该节的虚拟终点（不含）
};

// 成功时把结果写进 `out` 并返回 true。失败时返回 false，`error` 是一行排障说明，
// 失败分类写进 `kind`（可为 nullptr）。
bool pe_export_rva(std::wstring const& file_path, char const* export_name, PeExportInfo& out,
                   std::wstring& error, PeExportError* kind = nullptr);

// 磁盘镜像与目标进程里那个模块是不是同一份：`remote_image_size` 是 `MODULEENTRY32W` 的
// `modBaseSize`（等于 SizeOfImage）。对不上说明目标装载的是另一个版本，偏移不再有意义。
//
// 注意：**这只够当快速前置**。SizeOfImage 相同不等于同一个 build；真正允许调用远程导出之前，
// 还必须走 `remote.h` 的 `remote_image_identity` + `image_identity_matches`（核验 Machine /
// PE32+ / TimeDateStamp / 同名导出的**远程真实 RVA** / 该 RVA 落在可执行节内），并且调用时用
// 远程确认过的 RVA，而不是这里的磁盘 RVA。
bool pe_image_matches_module(PeExportInfo const& info, unsigned long long remote_image_size,
                             std::wstring& error);

}  // namespace deskfeed::inject
