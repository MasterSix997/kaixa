#include <test_generated_math/math.hpp>
#include <test_generated_support/support.hpp>

int main() {
    const int value = test_generated_math::add(20, 22);
    return value == test_generated_support::value() ? 0 : 1;
}
