#include <adapter_example/calculator.hpp>

namespace adapter_example {
    int add(const int left, const int right) {
        return left + right;
    }

    int multiply(const int left, const int right) {
        return left * right;
    }
}
