// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstring>

#include "tests/test.h"

namespace modemd::test {

namespace {
bool currentFailed = false;
}

std::vector<Case> &cases()
{
    static std::vector<Case> all;
    return all;
}

void fail(const char *file, int line, const char *expression)
{
    std::printf("    %s:%d: CHECK(%s) failed\n", file, line, expression);
    currentFailed = true;
}

bool failed()
{
    return currentFailed;
}

}  // namespace modemd::test

int main(int argc, char **argv)
{
    using namespace modemd::test;
    int failures = 0, run = 0;

    for (auto &c : cases()) {
        if (argc > 1 && !std::strstr(c.name, argv[1]))
            continue;
        currentFailed = false;
        c.body();
        run++;
        std::printf("[%s] %s\n", currentFailed ? "FAIL" : " OK ", c.name);
        failures += currentFailed;
    }

    std::printf("%d tests, %d failed\n", run, failures);
    return failures ? 1 : 0;
}
