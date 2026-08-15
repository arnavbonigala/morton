#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace morton_test {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failure_count() {
    static int count = 0;
    return count;
}

inline const char*& current_case() {
    static const char* name = "";
    return name;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void report_failure(const char* file, int line, const std::string& message) {
    ++failure_count();
    std::printf("  FAIL %s:%d\n       %s\n", file, line, message.c_str());
}

inline bool nearly_equal(double a, double b, double tolerance) {
    return std::fabs(a - b) <= tolerance;
}

inline int run_all() {
    int failed_cases = 0;
    for (const Case& c : registry()) {
        current_case() = c.name;
        int before = failure_count();
        std::printf("[ RUN ] %s\n", c.name);
        c.fn();
        if (failure_count() > before) {
            ++failed_cases;
            std::printf("[FAIL] %s\n", c.name);
        } else {
            std::printf("[ OK ] %s\n", c.name);
        }
    }
    std::printf("\n%d/%zu cases passed, %d assertion failures\n",
                static_cast<int>(registry().size()) - failed_cases, registry().size(),
                failure_count());
    return failed_cases == 0 ? 0 : 1;
}

}  // namespace morton_test

#define TEST_CASE(name)                                                            \
    static void name();                                                            \
    static ::morton_test::Registrar registrar_##name(#name, &name);                \
    static void name()

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) ::morton_test::report_failure(__FILE__, __LINE__, "expected: " #cond); \
    } while (0)

#define CHECK_EQ(a, b)                                                             \
    do {                                                                           \
        auto lhs_ = (a);                                                           \
        auto rhs_ = (b);                                                           \
        if (!(lhs_ == rhs_)) {                                                     \
            ::morton_test::report_failure(__FILE__, __LINE__,                      \
                                          std::string(#a " == " #b " | got ") +    \
                                              std::to_string(lhs_) + " vs " +      \
                                              std::to_string(rhs_));               \
        }                                                                          \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                      \
    do {                                                                           \
        double lhs_ = static_cast<double>(a);                                      \
        double rhs_ = static_cast<double>(b);                                      \
        if (!::morton_test::nearly_equal(lhs_, rhs_, tol)) {                       \
            ::morton_test::report_failure(__FILE__, __LINE__,                      \
                                          std::string(#a " ~= " #b " | got ") +    \
                                              std::to_string(lhs_) + " vs " +      \
                                              std::to_string(rhs_));               \
        }                                                                          \
    } while (0)

#define TEST_MAIN()                                                                \
    int main() { return ::morton_test::run_all(); }
