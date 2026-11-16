// SPDX-License-Identifier: GPL-2.0-or-later
//
// A minimal test runner: TEST(name) { ... CHECK(cond); ... }
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace modemd::test {

struct Case {
    const char *name;
    std::function<void()> body;
};

std::vector<Case> &cases();
void fail(const char *file, int line, const char *expression);
bool failed();

struct Register {
    Register(const char *name, std::function<void()> body) { cases().push_back({name, std::move(body)}); }
};

}  // namespace modemd::test

#define TEST(name)                                                               \
    static void test_##name();                                                   \
    static ::modemd::test::Register register_##name(#name, test_##name);        \
    static void test_##name()

#define CHECK(expression)                                                        \
    do {                                                                         \
        if (!(expression)) {                                                     \
            ::modemd::test::fail(__FILE__, __LINE__, #expression);               \
            return;                                                              \
        }                                                                        \
    } while (0)
