#pragma once

#include <string>

// URL 解析与重定向解析。
//
// 只接受 http / https 两种协议；主机名必须存在。这两条是协议的一部分：桌面端只对
// 保存下来的地址发 GET，重定向由主进程自己跟着走（见 docs/protocol.md 的“HTTP 规则”），
// 所以“下一个地址是什么”必须由这里算清楚，而不是交给 WinHTTP 的黑盒策略。
namespace deskfeed {

struct UrlParts {
    std::wstring scheme;  // 小写，"http" 或 "https"
    std::wstring host;
    unsigned short port = 0;
    std::wstring path;  // 以 '/' 开头（没有路径时补成 "/"），不含片段
    bool secure = false;
};

// 解析失败（协议不对、没有主机名）返回 false，error 是面向排障的一句话。
bool url_crack(std::wstring const& url, UrlParts& out, std::wstring& error);

// 按 RFC 3986 的相对解析把 Location 叠到当前地址上。只在“有界重定向”里使用：
// 不改写保存的配置地址，只用于本次请求。
bool url_resolve(std::wstring const& base, std::wstring const& reference, std::wstring& out,
                 std::wstring& error);

// 用于日志与 --inspect 输出的脱敏地址：去掉 userinfo（含密码）、查询串与片段，
// 只留 scheme://host[:port]/path。地址里可能带着上游密钥（userinfo 或 ?token=...），
// 所以任何打印出去的地方都必须先过这一层。
// removed 非空时写出“是否真的删掉了东西”，供 runtimeUrlRedacted 之类的字段使用。
// **实际保存与请求用的地址一个字节都不改**。
std::wstring url_redact(std::wstring const& url, bool* removed = nullptr);

}  // namespace deskfeed
