#pragma once

#include <string>

// 标准输出/标准错误的写入。控制台走宽字符接口（中文不会变成乱码），
// 重定向到文件或管道时写原始 UTF-8 字节。
namespace deskfeed {

void console_write_stdout(std::string const& utf8);
void console_write_stderr(std::string const& utf8);
void console_write_stderr(std::wstring const& text);

}  // namespace deskfeed
