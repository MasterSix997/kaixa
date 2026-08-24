#include <runtime_support/value.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main(const int argument_count, const char* arguments[]) {
    if (argument_count == 0)
        return 1;

    const std::filesystem::path runtime_file = std::filesystem::absolute(arguments[0]).parent_path() / "runtime.dat";
    std::ifstream input(runtime_file);
    std::string runtime_value;
    std::getline(input, runtime_value);
    if (!input && runtime_value.empty())
        return 2;

    std::cout << runtime_support::value() << ' ' << runtime_value << '\n';
    return 0;
}
