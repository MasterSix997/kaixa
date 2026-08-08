#include <test_support.hpp>

#include <iostream>
#include <string_view>

int main(const int argc, char** argv) {
    if (argc >= 2 && std::string_view(argv[1]) == kaixa::testing::echo_flag)
        return kaixa::testing::run_echo_mode(argc, argv);
    return kaixa::testing::TestRegistry::instance().run_all(std::cout);
}
