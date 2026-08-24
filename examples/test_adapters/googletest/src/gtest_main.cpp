#include <gtest/gtest.h>

#include <iostream>
#include <string_view>

namespace testing {
    std::vector<TestCase>& registry() {
        static std::vector<TestCase> tests;
        return tests;
    }

    Registrar::Registrar(const char* suite, const char* name, void (*run)()) {
        registry().push_back({suite, name, run});
    }
}

int main(const int argument_count, const char* arguments[]) {
    bool list = false;
    std::string_view filter = "*";
    for (int index = 1; index < argument_count; ++index) {
        const std::string_view argument = arguments[index];
        if (argument == "--gtest_list_tests")
            list = true;

        constexpr std::string_view prefix = "--gtest_filter=";
        if (argument.starts_with(prefix))
            filter = argument.substr(prefix.size());
    }

    if (list) {
        std::string_view suite;
        for (const testing::TestCase& test: testing::registry()) {
            if (suite != test.suite) {
                suite = test.suite;
                std::cout << suite << ".\n";
            }
            std::cout << "  " << test.name << '\n';
        }
        return 0;
    }

    int failures = 0;
    for (const testing::TestCase& test: testing::registry()) {
        const std::string name = test.suite + "." + test.name;
        if (filter != "*" && filter != name)
            continue;

        try {
            test.run();
            std::cout << "[       OK ] " << name << '\n';
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[  FAILED  ] " << name << ": " << exception.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
