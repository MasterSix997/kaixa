#include <test_generated_math/math.hpp>
#include <test_generated_support/support.hpp>

#include <iostream>
#include <string_view>

namespace {
    int run_answer_test() {
        const int value = test_generated_math::add(20, 22);
        return value == test_generated_support::value() ? 0 : 1;
    }
}

int main(const int argc, char** argv) {
    if (argc >= 2 && std::string_view(argv[1]) == "--kaixa-test-list") {
        std::cout << "answer\n";
        return 0;
    }

    if (argc >= 3 && std::string_view(argv[1]) == "--kaixa-test-run") {
        if (std::string_view(argv[2]) == "answer")
            return run_answer_test();

        std::cerr << "unknown test: " << argv[2] << '\n';
        return 2;
    }

    if (argc >= 2) {
        std::cerr << "unknown argument: " << argv[1] << '\n';
        return 2;
    }

    return run_answer_test();
}
