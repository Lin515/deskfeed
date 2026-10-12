#pragma once

#include <windows.h>

#include <sstream>
#include <string>
#include <vector>

// 极简测试框架：不引第三方依赖，避免公开工程为了几个断言拖进一整套测试生态。
//
// 用法：
//   TEST(name) { CHECK(condition); CHECK_EQ(actual, expected); }
// 断言失败会抛 Failure，由 main 捕获并把当前用例记为失败；一个用例失败不影响其它用例。
namespace testing {

struct Failure {
    std::string message;
};

using Body = void (*)();

struct Case {
    char const* name;
    Body body;
};

std::vector<Case>& cases();

struct Registrar {
    Registrar(char const* name, Body body) { cases().push_back(Case{name, body}); }
};

[[noreturn]] void report_failure(std::string message);

inline std::string to_display(std::wstring const& value) {
    if (value.empty()) {
        return std::string();
    }
    int const bytes = ::WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                            static_cast<int>(value.size()), nullptr, 0, nullptr,
                                            nullptr);
    if (bytes <= 0) {
        return std::string("<invalid wide string>");
    }
    std::string out(static_cast<std::size_t>(bytes), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(),
                          bytes, nullptr, nullptr);
    return out;
}

template <class T>
std::string to_display(T const& value) {
    std::ostringstream os;
    os << value;
    return os.str();
}

template <class A, class E>
void check_equal(A const& actual, E const& expected, char const* actual_text,
                 char const* expected_text, char const* file, int line) {
    if (actual == expected) {
        return;
    }
    std::ostringstream os;
    os << file << ":" << line << ": " << actual_text << " != " << expected_text << " ("
       << to_display(actual) << " vs " << to_display(expected) << ")";
    report_failure(os.str());
}

}  // namespace testing

#define TEST(name)                                                    \
    static void name##_test_body();                                   \
    static ::testing::Registrar name##_registrar(#name, &name##_test_body); \
    static void name##_test_body()

#define CHECK(expression)                                                        \
    do {                                                                         \
        if (!(expression)) {                                                     \
            ::testing::report_failure(std::string(__FILE__) + ":" +              \
                                      std::to_string(__LINE__) + ": " + #expression); \
        }                                                                        \
    } while (false)

#define CHECK_EQ(actual, expected) \
    ::testing::check_equal((actual), (expected), #actual, #expected, __FILE__, __LINE__)
