#pragma once

#include <cmath>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace kotone::test {

struct TestContext {
    int failures = 0;
    std::string current_test;
};

inline TestContext& ctx() {
    static TestContext instance;
    return instance;
}

inline void fail(const char* file, int line, const std::string& msg) {
    ++ctx().failures;
    std::cerr << "  [FAIL] " << ctx().current_test << " (" << file << ":" << line << "): " << msg
              << "\n";
}

#define KOTONE_EXPECT_TRUE(cond)                                                                   \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ::kotone::test::fail(__FILE__, __LINE__, std::string("Expected true: ") + #cond);      \
        }                                                                                          \
    } while (0)

#define KOTONE_EXPECT_FALSE(cond)                                                                  \
    do {                                                                                           \
        if ((cond)) {                                                                              \
            ::kotone::test::fail(__FILE__, __LINE__, std::string("Expected false: ") + #cond);     \
        }                                                                                          \
    } while (0)

#define KOTONE_EXPECT_EQ(a, b)                                                                     \
    do {                                                                                           \
        const auto& _va = (a);                                                                     \
        const auto& _vb = (b);                                                                     \
        if (!(_va == _vb)) {                                                                       \
            std::ostringstream _oss;                                                               \
            _oss << "Expected " << #a << " == " << #b << " (got " << _va << " vs " << _vb << ")";  \
            ::kotone::test::fail(__FILE__, __LINE__, _oss.str());                                  \
        }                                                                                          \
    } while (0)

#define KOTONE_EXPECT_NEAR(a, b, tol)                                                              \
    do {                                                                                           \
        const double _va = static_cast<double>(a);                                                 \
        const double _vb = static_cast<double>(b);                                                 \
        const double _vt = static_cast<double>(tol);                                               \
        if (!std::isfinite(_va) || !std::isfinite(_vb) || std::abs(_va - _vb) > _vt) {             \
            std::ostringstream _oss;                                                               \
            _oss << "Expected |" << #a << " - " << #b << "| <= " << _vt << " (got " << _va         \
                 << " vs " << _vb << ", diff=" << std::abs(_va - _vb) << ")";                      \
            ::kotone::test::fail(__FILE__, __LINE__, _oss.str());                                  \
        }                                                                                          \
    } while (0)

#define KOTONE_EXPECT_LE(a, b)                                                                     \
    do {                                                                                           \
        const double _va = static_cast<double>(a);                                                 \
        const double _vb = static_cast<double>(b);                                                 \
        if (!std::isfinite(_va) || !std::isfinite(_vb) || !(_va <= _vb)) {                         \
            std::ostringstream _oss;                                                               \
            _oss << "Expected " << #a << " <= " << #b << " (got " << _va << " vs " << _vb << ")";  \
            ::kotone::test::fail(__FILE__, __LINE__, _oss.str());                                  \
        }                                                                                          \
    } while (0)

#define KOTONE_EXPECT_GE(a, b)                                                                     \
    do {                                                                                           \
        const double _va = static_cast<double>(a);                                                 \
        const double _vb = static_cast<double>(b);                                                 \
        if (!std::isfinite(_va) || !std::isfinite(_vb) || !(_va >= _vb)) {                         \
            std::ostringstream _oss;                                                               \
            _oss << "Expected " << #a << " >= " << #b << " (got " << _va << " vs " << _vb << ")";  \
            ::kotone::test::fail(__FILE__, __LINE__, _oss.str());                                  \
        }                                                                                          \
    } while (0)

void run_dsp_limiter_tests(int& passed, int& failed);
void run_dsp_eq_comp_tests(int& passed, int& failed);
void run_profile_loader_tests(int& passed, int& failed);

template <typename Fn> inline void run_case(const char* name, Fn&& fn, int& passed, int& failed) {
    ctx().current_test = name;
    const int before = ctx().failures;
    fn();
    if (ctx().failures == before) {
        ++passed;
        std::cout << "[PASS] " << name << "\n";
    } else {
        ++failed;
        std::cerr << "[FAIL] " << name << "\n";
    }
}

} // namespace kotone::test
