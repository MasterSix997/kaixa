#pragma once

#include <stdexcept>
#include <string>
#include <vector>

namespace testing {
    struct TestCase {
        std::string suite;
        std::string name;
        void (*run)();
    };

    std::vector<TestCase>& registry();

    struct Registrar {
        Registrar(const char* suite, const char* name, void (*run)());
    };

    template <typename Left, typename Right> void expect_equal(const Left& left, const Right& right) {
        if (!(left == right))
            throw std::runtime_error("EXPECT_EQ failed");
    }
}

#define KAIXA_GTEST_JOIN_INNER(left, right) left##right
#define KAIXA_GTEST_JOIN(left, right) KAIXA_GTEST_JOIN_INNER(left, right)
#define TEST(suite, name)                                                                                                                  \
    static void KAIXA_GTEST_JOIN(suite##_##name##_test_, __LINE__)();                                                                      \
    static ::testing::Registrar KAIXA_GTEST_JOIN(suite##_##name##_registrar_, __LINE__)(                                                   \
        #suite,                                                                                                                            \
        #name,                                                                                                                             \
        &KAIXA_GTEST_JOIN(suite##_##name##_test_, __LINE__)                                                                                \
    );                                                                                                                                     \
    static void KAIXA_GTEST_JOIN(suite##_##name##_test_, __LINE__)()
#define EXPECT_EQ(left, right) ::testing::expect_equal((left), (right))
