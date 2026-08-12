#include "application.hpp"
#include "command_line.hpp"

#include <iostream>
#include <string_view>
#include <vector>

int main(const int argc, char** argv) {
    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<std::size_t>(argc - 1));
    for (int index = 1; index < argc; ++index)
        arguments.emplace_back(argv[index]);

    auto command = kaixa::cli::parse_command_line(arguments);
    if (!command) {
        std::cerr << "error: " << command.error().message << '\n';
        if (command.error().show_usage)
            kaixa::cli::print_usage(std::cout);

        return 2;
    }

    return kaixa::cli::execute(*command);
}
