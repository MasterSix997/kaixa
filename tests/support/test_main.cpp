#include <test_support.hpp>

#include <iostream>
#include <string_view>

int main(const int argc, char** argv) {
    kaixa::testing::TestRegistry& registry = kaixa::testing::TestRegistry::instance();
    if (argc >= 2) {
        const std::string_view argument = argv[1];
        if (argument == kaixa::testing::echo_flag)
            return kaixa::testing::run_echo_mode(argc, argv);

        if (argument == kaixa::testing::list_flag) {
            registry.list(std::cout);
            return 0;
        }

        if (argument == kaixa::testing::run_flag) {
            if (argc < 3) {
                std::cerr << kaixa::testing::run_flag << " requires a test name\n";
                return 2;
            }

            return registry.run(argv[2], std::cout);
        }
    }

    return registry.run_all(std::cout);
}
