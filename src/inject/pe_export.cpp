#include "inject/pe_export.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

namespace deskfeed::inject {
namespace {

// 节表里的一项。RVA→文件偏移只按 raw_size 判界：我们读的是**文件里的字节**，一个节在文件里
// 真正有的数据就是 raw_size 那么多，virtual_size 之外的部分（BSS）在文件里根本不存在。
struct Section {
    unsigned long virtual_address = 0;
    unsigned long virtual_size = 0;
    unsigned long raw_offset = 0;
    unsigned long raw_size = 0;
    unsigned long characteristics = 0;

    bool executable() const {
        return (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    }
};

struct PeFile {
    unsigned char const* bytes = nullptr;
    unsigned long long size = 0;
    unsigned short machine = 0;
    unsigned long long image_size = 0;
    unsigned long time_date_stamp = 0;  // COFF TimeDateStamp：换 build 的强区分位
    bool pe32_plus = false;
    std::vector<Section> sections;

    // 把一段 RVA 区间映射成文件偏移；不在任何节的文件数据里时返回 false。
    // `section_remaining`（可选）给出从映射点起到该节原始数据末尾还有多少字节——用来限制
    // "读一个以 NUL 结尾的串"的扫描范围，绝不跨到相邻节的字节里。
    bool map(unsigned long rva, unsigned long long need, unsigned long long& offset,
             unsigned long long* section_remaining = nullptr) const {
        for (Section const& section : sections) {
            if (rva < section.virtual_address) {
                continue;
            }
            unsigned long long const delta = rva - section.virtual_address;
            if (delta >= section.raw_size) {
                continue;
            }
            if (delta + need > section.raw_size) {
                return false;  // 这次读取会越过本节在文件里的数据
            }
            unsigned long long const at = static_cast<unsigned long long>(section.raw_offset) + delta;
            if (at + need > size) {
                return false;  // 声明越过了文件末尾：坏头，不读
            }
            offset = at;
            if (section_remaining != nullptr) {
                *section_remaining = section.raw_size - delta;
            }
            return true;
        }
        return false;
    }

    // 某个 RVA 落在哪个节里（按节的**虚拟**范围判断，用于"这个函数在可执行节里吗"）。
    Section const* section_of_rva(unsigned long rva) const {
        for (Section const& section : sections) {
            unsigned long long const span = section.virtual_size;
            if (rva >= section.virtual_address && rva < section.virtual_address + span) {
                return &section;
            }
        }
        return nullptr;
    }

    template <class T>
    bool read(unsigned long long offset, T& out) const {
        if (offset + sizeof(T) > size) {
            return false;
        }
        std::memcpy(&out, bytes + offset, sizeof(T));
        return true;
    }

    // 读一个以 NUL 结尾的 ASCII 串；NUL 必须落在 `limit` 字节以内（调用方传的是本节剩余量）。
    bool read_c_string(unsigned long long offset, unsigned long long limit, std::string_view& out) const {
        if (offset >= size || limit == 0) {
            return false;
        }
        unsigned long long const available = size - offset;
        unsigned long long const span = available < limit ? available : limit;
        auto const* begin = reinterpret_cast<char const*>(bytes + offset);
        char const* end = static_cast<char const*>(std::memchr(begin, '\0', static_cast<size_t>(span)));
        if (end == nullptr) {
            return false;
        }
        out = std::string_view(begin, static_cast<size_t>(end - begin));
        return true;
    }
};

bool fail(PeExportError kind, std::wstring message, std::wstring& error, PeExportError* out_kind) {
    error = std::move(message);
    if (out_kind != nullptr) {
        *out_kind = kind;
    }
    return false;
}

class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile() {
        if (view_ != nullptr) {
            ::UnmapViewOfFile(view_);
        }
        if (mapping_ != nullptr) {
            ::CloseHandle(mapping_);
        }
        if (file_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(file_);
        }
    }
    MappedFile(MappedFile const&) = delete;
    MappedFile& operator=(MappedFile const&) = delete;

    bool open(std::wstring const& path, std::wstring& error) {
        file_ = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            error = L"cannot open " + path;
            return false;
        }
        LARGE_INTEGER length{};
        if (!::GetFileSizeEx(file_, &length)) {
            error = L"cannot size " + path;
            return false;
        }
        size_ = static_cast<unsigned long long>(length.QuadPart);
        if (size_ == 0) {
            error = path + L" is empty";
            return false;
        }
        mapping_ = ::CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping_ == nullptr) {
            error = L"cannot map " + path;
            return false;
        }
        view_ = ::MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0);
        if (view_ == nullptr) {
            error = L"cannot view " + path;
            return false;
        }
        return true;
    }

    unsigned char const* bytes() const { return static_cast<unsigned char const*>(view_); }
    unsigned long long size() const { return size_; }

private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
    void* view_ = nullptr;
    unsigned long long size_ = 0;
};

// 头解析：返回 false 时 error/kind 已写好。导出目录的 RVA/大小通过出参交给调用方。
bool parse_headers(PeFile& pe, unsigned long& export_rva, unsigned long& export_size,
                   std::wstring& error, PeExportError* kind) {
    if (pe.size < 0x40) {
        return fail(PeExportError::NotPe, L"the file is too small to be a PE image", error, kind);
    }
    if (pe.bytes[0] != 'M' || pe.bytes[1] != 'Z') {
        return fail(PeExportError::NotPe, L"missing the MZ signature", error, kind);
    }

    LONG pe_offset = 0;
    if (!pe.read(0x3C, pe_offset) || pe_offset <= 0) {
        return fail(PeExportError::BadHeader, L"invalid e_lfanew", error, kind);
    }
    unsigned long long const nt = static_cast<unsigned long long>(pe_offset);
    if (nt + 0x18 > pe.size) {
        return fail(PeExportError::BadHeader, L"the NT headers extend past the end of the file", error,
                    kind);
    }
    if (std::memcmp(pe.bytes + nt, "PE\0\0", 4) != 0) {
        return fail(PeExportError::NotPe, L"missing the PE signature", error, kind);
    }

    unsigned short section_count = 0;
    unsigned short optional_size = 0;
    if (!pe.read(nt + 4, pe.machine) || !pe.read(nt + 4 + 2, section_count) ||
        !pe.read(nt + 4 + 4, pe.time_date_stamp) || !pe.read(nt + 4 + 16, optional_size)) {
        return fail(PeExportError::BadHeader, L"cannot read the COFF header", error, kind);
    }
    // 本工程只有 x64：负载要和 64 位 explorer.exe 同位数，注入器也只解析自己那一份 DLL。
    // 非 AMD64 的镜像一律拒绝——包括 PE32（0x14c）与 ARM64（0xaa64）。
    if (pe.machine != IMAGE_FILE_MACHINE_AMD64) {
        return fail(PeExportError::UnsupportedMachine,
                    L"the image is not AMD64 (machine 0x" + std::to_wstring(pe.machine) + L")",
                    error, kind);
    }

    unsigned long long const optional = nt + 4 + 20;
    if (optional + optional_size > pe.size) {
        return fail(PeExportError::BadHeader, L"the optional header extends past the end of the file",
                    error, kind);
    }

    unsigned short magic = 0;
    if (!pe.read(optional, magic)) {
        return fail(PeExportError::BadHeader, L"cannot read the optional header magic", error, kind);
    }
    // DataDirectory 在 PE32+ 里从 112 开始，在 PE32 里从 96 开始；导出目录是第 0 项。
    // 这里只接受 PE32+：机器类型已经是 AMD64 了，magic 也必须对上。
    if (magic != 0x20B) {
        return fail(PeExportError::UnsupportedMachine, L"the optional header is not PE32+", error,
                    kind);
    }
    pe.pe32_plus = true;
    unsigned long long const data_directory = 112;

    unsigned long size_of_image = 0;
    unsigned long number_of_directories = 0;
    if (!pe.read(optional + 56, size_of_image) || !pe.read(optional + 108, number_of_directories)) {
        return fail(PeExportError::BadHeader, L"cannot read SizeOfImage/NumberOfRvaAndSizes", error,
                    kind);
    }
    if (size_of_image == 0) {
        return fail(PeExportError::BadHeader, L"SizeOfImage is zero", error, kind);
    }
    if (number_of_directories < 1) {
        return fail(PeExportError::NoExportDirectory, L"the optional header declares no data "
                                                      L"directories",
                    error, kind);
    }
    if (data_directory + 8 > optional_size) {
        return fail(PeExportError::BadHeader, L"the optional header has no room for a data directory",
                    error, kind);
    }
    if (!pe.read(optional + data_directory, export_rva) ||
        !pe.read(optional + data_directory + 4, export_size)) {
        return fail(PeExportError::BadHeader, L"cannot read the export directory entry", error, kind);
    }
    if (export_rva == 0 || export_size == 0) {
        return fail(PeExportError::NoExportDirectory, L"the image has no export directory", error,
                    kind);
    }
    pe.image_size = size_of_image;

    unsigned long long const sections = optional + optional_size;
    for (unsigned short index = 0; index < section_count; ++index) {
        unsigned long long const entry = sections + static_cast<unsigned long long>(index) * 40;
        if (entry + 40 > pe.size) {
            return fail(PeExportError::BadHeader, L"a section header extends past the end of the file",
                        error, kind);
        }
        Section section;
        if (!pe.read(entry + 8, section.virtual_size) ||
            !pe.read(entry + 12, section.virtual_address) ||
            !pe.read(entry + 16, section.raw_size) || !pe.read(entry + 20, section.raw_offset) ||
            !pe.read(entry + 36, section.characteristics)) {
            return fail(PeExportError::BadHeader, L"cannot read a section header", error, kind);
        }
        pe.sections.push_back(section);
    }
    if (pe.sections.empty()) {
        return fail(PeExportError::BadHeader, L"the image has no sections", error, kind);
    }
    return true;
}

}  // namespace

char const* pe_export_error_name(PeExportError kind) {
    switch (kind) {
        case PeExportError::None: return "none";
        case PeExportError::OpenFailed: return "open-failed";
        case PeExportError::NotPe: return "not-pe";
        case PeExportError::BadHeader: return "bad-header";
        case PeExportError::UnsupportedMachine: return "unsupported-machine";
        case PeExportError::NoExportDirectory: return "no-export-directory";
        case PeExportError::TablesNotMapped: return "tables-not-mapped";
        case PeExportError::NotFound: return "not-found";
        case PeExportError::Forwarded: return "forwarded";
        case PeExportError::NotExecutable: return "not-executable";
    }
    return "unknown";
}

bool pe_export_rva(std::wstring const& file_path, char const* export_name, PeExportInfo& out,
                   std::wstring& error, PeExportError* kind) {
    out = PeExportInfo{};
    if (kind != nullptr) {
        *kind = PeExportError::None;
    }
    if (export_name == nullptr || *export_name == '\0') {
        return fail(PeExportError::NotFound, L"no export name was given", error, kind);
    }

    MappedFile file;
    std::wstring open_error;
    if (!file.open(file_path, open_error)) {
        return fail(PeExportError::OpenFailed, std::move(open_error), error, kind);
    }

    PeFile pe;
    pe.bytes = file.bytes();
    pe.size = file.size();

    unsigned long export_rva = 0;
    unsigned long export_size = 0;
    if (!parse_headers(pe, export_rva, export_size, error, kind)) {
        return false;
    }

    // 导出目录必须整体落在 image 里，也必须落在一个节的文件数据里。用 64 位加法：两个 DWORD
    // 相加可以在 32 位里回绕，从而"绕过"范围检查。
    unsigned long long const directory_begin = export_rva;
    unsigned long long const directory_end = directory_begin + export_size;
    if (directory_end > pe.image_size) {
        return fail(PeExportError::BadHeader, L"the export directory extends past SizeOfImage", error,
                    kind);
    }

    unsigned long long directory_offset = 0;
    if (!pe.map(export_rva, 40, directory_offset)) {
        return fail(PeExportError::TablesNotMapped, L"the export directory is not in any section",
                    error, kind);
    }

    unsigned long function_count = 0;
    unsigned long name_count = 0;
    unsigned long functions_rva = 0;
    unsigned long names_rva = 0;
    unsigned long ordinals_rva = 0;
    if (!pe.read(directory_offset + 20, function_count) ||
        !pe.read(directory_offset + 24, name_count) ||
        !pe.read(directory_offset + 28, functions_rva) ||
        !pe.read(directory_offset + 32, names_rva) ||
        !pe.read(directory_offset + 36, ordinals_rva)) {
        return fail(PeExportError::BadHeader, L"cannot read the export directory fields", error, kind);
    }

    unsigned long long names_offset = 0;
    unsigned long long ordinals_offset = 0;
    unsigned long long functions_offset = 0;
    if (!pe.map(names_rva, static_cast<unsigned long long>(name_count) * 4, names_offset) ||
        !pe.map(ordinals_rva, static_cast<unsigned long long>(name_count) * 2, ordinals_offset) ||
        !pe.map(functions_rva, static_cast<unsigned long long>(function_count) * 4, functions_offset)) {
        return fail(PeExportError::TablesNotMapped,
                    L"the export name/ordinal/function tables are not in any section", error, kind);
    }

    for (unsigned long index = 0; index < name_count; ++index) {
        unsigned long name_rva = 0;
        if (!pe.read(names_offset + static_cast<unsigned long long>(index) * 4, name_rva)) {
            break;
        }
        // 名字串的 NUL 只能在**它所在那一个节**的剩余原始数据里找，绝不跨到相邻节去"读名字"。
        unsigned long long name_offset = 0;
        unsigned long long name_remaining = 0;
        std::string_view candidate;
        if (!pe.map(name_rva, 1, name_offset, &name_remaining) ||
            !pe.read_c_string(name_offset, name_remaining, candidate)) {
            continue;
        }
        if (candidate != export_name) {
            continue;
        }

        unsigned short ordinal = 0;
        if (!pe.read(ordinals_offset + static_cast<unsigned long long>(index) * 2, ordinal)) {
            break;
        }
        if (ordinal >= function_count) {
            return fail(PeExportError::BadHeader, L"the export ordinal is out of range", error, kind);
        }
        unsigned long function_rva = 0;
        if (!pe.read(functions_offset + static_cast<unsigned long long>(ordinal) * 4, function_rva)) {
            return fail(PeExportError::BadHeader, L"the export function entry is out of range", error,
                        kind);
        }

        // 转发导出：RVA 落在导出目录自身里（64 位比较，避免 DWORD 回绕绕过），那里存的是
        // "DLL.函数" 字符串而不是代码。把它当远程线程入口会让目标进程在数据上执行。
        if (static_cast<unsigned long long>(function_rva) >= directory_begin &&
            static_cast<unsigned long long>(function_rva) < directory_end) {
            std::wstring target;
            unsigned long long forward_offset = 0;
            unsigned long long forward_remaining = 0;
            std::string_view forward;
            if (pe.map(function_rva, 1, forward_offset, &forward_remaining) &&
                pe.read_c_string(forward_offset, forward_remaining, forward)) {
                target.assign(forward.begin(), forward.end());
            }
            return fail(PeExportError::Forwarded,
                        L"export " + std::wstring(export_name, export_name + std::strlen(export_name)) +
                            L" is a forwarder" + (target.empty() ? L"" : L" (to " + target + L")") +
                            L"; refusing to call it",
                        error, kind);
        }

        // RVA 0 = "没有这个函数"；越过 image 或不在可执行节里都意味着远程执行会跳进数据。
        std::string const name_text(export_name, export_name + std::strlen(export_name));
        std::wstring const wide_name(name_text.begin(), name_text.end());
        if (function_rva == 0) {
            return fail(PeExportError::NotExecutable, L"export " + wide_name + L" has RVA 0", error,
                        kind);
        }
        if (static_cast<unsigned long long>(function_rva) >= pe.image_size) {
            return fail(PeExportError::NotExecutable,
                        L"export " + wide_name + L" points past SizeOfImage", error, kind);
        }
        Section const* const section = pe.section_of_rva(function_rva);
        if (section == nullptr) {
            return fail(PeExportError::NotExecutable,
                        L"export " + wide_name + L" is not inside any section", error, kind);
        }
        if (!section->executable()) {
            return fail(PeExportError::NotExecutable,
                        L"export " + wide_name + L" lives in a non-executable section (it is data, "
                        L"not a function entry)",
                        error, kind);
        }
        if (static_cast<unsigned long long>(section->virtual_address) + section->virtual_size >
            pe.image_size) {
            return fail(PeExportError::BadHeader,
                        L"the section that holds " + wide_name + L" extends past SizeOfImage", error,
                        kind);
        }

        out.rva = function_rva;
        out.image_size = pe.image_size;
        out.machine = pe.machine;
        // 换 build 的区分位与"这个 RVA 落在哪个可执行节"一起交出去：远程侧要用它们做一致性核验。
        out.time_date_stamp = pe.time_date_stamp;
        out.is_pe32_plus = pe.pe32_plus;
        out.section_va = section->virtual_address;
        out.section_end = static_cast<unsigned long long>(section->virtual_address) +
                          section->virtual_size;
        return true;
    }

    std::string const wanted(export_name, export_name + std::strlen(export_name));
    return fail(PeExportError::NotFound,
                L"export not found: " + std::wstring(wanted.begin(), wanted.end()), error, kind);
}

namespace {
// 十六进制文本（诊断用）。这一层不拉 platform 的头，所以就地写一个几行的实现。
std::wstring hex_of(unsigned long long value) {
    wchar_t buffer[32]{};
    ::swprintf_s(buffer, L"0x%llX", value);
    return std::wstring(buffer);
}
}  // namespace

bool pe_image_matches_module(PeExportInfo const& info, unsigned long long remote_image_size,
                             std::wstring& error) {
    if (remote_image_size == 0) {
        error = L"the target module reports a zero image size";
        return false;
    }
    if (info.image_size != remote_image_size) {
        error = L"the on-disk image (SizeOfImage " + hex_of(info.image_size) +
                L") does not match the loaded module (" + hex_of(remote_image_size) +
                L"); refusing to use an offset from a different build";
        return false;
    }
    return true;
}

}  // namespace deskfeed::inject
