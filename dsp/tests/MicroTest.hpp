// MicroTest.hpp — a 100-line zero-dependency test harness.
// Lets us verify the DSP in any C++17 environment (sandbox, CI, anywhere).
// Will be replaced by Catch2 in the on-Mac Xcode build, but the assertions
// are written generically so swapping in Catch2 later is a one-line change.

#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace microtest {

struct Failure {
    std::string testName;
    std::string file;
    int line;
    std::string what;
};

class Registry {
public:
    static Registry& instance() {
        static Registry r;
        return r;
    }
    void add(const std::string& name, std::function<void()> fn) {
        tests_.push_back({name, std::move(fn)});
    }
    int runAll() {
        for (auto& t : tests_) {
            currentTest_ = t.name;
            std::size_t before = failures_.size();
            try {
                t.fn();
            } catch (const std::exception& e) {
                failures_.push_back({t.name, "", 0, std::string("exception: ") + e.what()});
            } catch (...) {
                failures_.push_back({t.name, "", 0, "unknown exception"});
            }
            if (failures_.size() == before) {
                std::printf("  [PASS] %s\n", t.name.c_str());
            } else {
                std::printf("  [FAIL] %s\n", t.name.c_str());
            }
        }
        std::printf("\n");
        if (failures_.empty()) {
            std::printf("All %d tests passed.\n", (int)tests_.size());
            return 0;
        }
        std::printf("%d / %d tests failed:\n", (int)failures_.size(), (int)tests_.size());
        for (const auto& f : failures_) {
            std::printf("  %s\n    %s:%d  %s\n",
                        f.testName.c_str(), f.file.c_str(), f.line, f.what.c_str());
        }
        return 1;
    }
    void recordFailure(const std::string& file, int line, const std::string& what) {
        failures_.push_back({currentTest_, file, line, what});
    }

private:
    struct Test { std::string name; std::function<void()> fn; };
    std::vector<Test> tests_;
    std::vector<Failure> failures_;
    std::string currentTest_;
};

struct AutoRegister {
    AutoRegister(const char* name, std::function<void()> fn) {
        Registry::instance().add(name, std::move(fn));
    }
};

}  // namespace microtest

#define MICRO_TEST(name)                                                \
    static void name();                                                 \
    static ::microtest::AutoRegister _reg_##name(#name, name);          \
    static void name()

#define REQUIRE(cond)                                                                 \
    do {                                                                              \
        if (!(cond)) {                                                                \
            ::microtest::Registry::instance().recordFailure(__FILE__, __LINE__,       \
                                                            "REQUIRE failed: " #cond); \
            return;                                                                   \
        }                                                                             \
    } while (0)

#define REQUIRE_NEAR(actual, expected, eps)                                              \
    do {                                                                                 \
        const double _a = (actual);                                                      \
        const double _e = (expected);                                                    \
        const double _d = std::fabs(_a - _e);                                            \
        if (!(_d <= (eps))) {                                                            \
            char buf[256];                                                               \
            std::snprintf(buf, sizeof(buf),                                              \
                "REQUIRE_NEAR failed: %s == %.12g, expected %.12g (|diff|=%.3g > %.3g)", \
                #actual, _a, _e, _d, (eps));                                             \
            ::microtest::Registry::instance().recordFailure(__FILE__, __LINE__, buf);    \
            return;                                                                      \
        }                                                                                \
    } while (0)
