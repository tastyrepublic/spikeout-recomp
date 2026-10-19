// Minimal self-contained test harness (no external dependencies).
#pragma once

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace test {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int n = 0;
    return n;
}

struct Register {
    Register(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

template <typename T>
void print(std::ostream& os, const T& v) {
    if constexpr (std::is_enum_v<T>)
        os << static_cast<long long>(v);
    else
        os << v;
}

template <typename A, typename B>
void check_eq(const A& a, const B& b, const char* expr_a, const char* expr_b, const char* file, int line) {
    if (a == b) return;
    std::ostringstream os;
    os << file << ":" << line << ": CHECK_EQ(" << expr_a << ", " << expr_b << ") failed\n    left:  ";
    print(os, a);
    os << "\n    right: ";
    print(os, b);
    os << "\n";
    std::fputs(os.str().c_str(), stderr);
    ++failures();
}

inline int run_all() {
    for (const Case& c : registry()) {
        const int before = failures();
        c.fn();
        std::printf("[%s] %s\n", failures() == before ? " ok " : "FAIL", c.name);
    }
    std::printf("%zu tests, %d failed checks\n", registry().size(), failures());
    return failures() == 0 ? 0 : 1;
}

} // namespace test

#define TEST_CONCAT_(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT_(a, b)
#define TEST_CASE(name)                                                              \
    static void TEST_CONCAT(test_fn_, __LINE__)();                                   \
    static const test::Register TEST_CONCAT(test_reg_, __LINE__)(name, TEST_CONCAT(test_fn_, __LINE__)); \
    static void TEST_CONCAT(test_fn_, __LINE__)()

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++test::failures();                                                      \
        }                                                                            \
    } while (0)

#define CHECK_EQ(a, b) test::check_eq((a), (b), #a, #b, __FILE__, __LINE__)
