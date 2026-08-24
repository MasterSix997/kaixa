#include <adapter_example/calculator.hpp>

#include <gtest/gtest.h>

TEST(Calculator, Adds) {
    EXPECT_EQ(adapter_example::add(20, 22), 42);
}

TEST(Calculator, Multiplies) {
    EXPECT_EQ(adapter_example::multiply(6, 7), 42);
}
