#include <benchmark/benchmark.h>

#include <iostream>
#include <string_view>

namespace benchmark {
    std::vector<BenchmarkCase>& registry() {
        static std::vector<BenchmarkCase> benchmarks;
        return benchmarks;
    }

    Registrar::Registrar(const char* name, void (*run)(State&))
        : m_index(registry().size()) {
        registry().push_back({name, run, {}});
    }

    Registrar* Registrar::Arg(const int argument) {
        registry()[m_index].arguments.push_back(argument);
        return this;
    }

    Registrar* register_benchmark(const char* name, void (*run)(State&)) {
        return new Registrar(name, run);
    }
}

int main(const int argument_count, const char* arguments[]) {
    bool list = false;
    std::string_view filter;
    for (int index = 1; index < argument_count; ++index) {
        const std::string_view argument = arguments[index];
        if (argument == "--benchmark_list_tests=true")
            list = true;

        constexpr std::string_view prefix = "--benchmark_filter=";
        if (argument.starts_with(prefix)) {
            filter = argument.substr(prefix.size());
            if (filter.starts_with('^'))
                filter.remove_prefix(1);
            if (filter.ends_with('$'))
                filter.remove_suffix(1);
        }
    }

    for (const benchmark::BenchmarkCase& benchmark: benchmark::registry()) {
        const std::vector<int> arguments = benchmark.arguments.empty() ? std::vector<int>{0} : benchmark.arguments;
        for (const int argument: arguments) {
            const std::string name = benchmark.name + (benchmark.arguments.empty() ? "" : "/" + std::to_string(argument));
            if (list) {
                std::cout << name << '\n';
                continue;
            }
            if (!filter.empty() && filter != name)
                continue;

            benchmark::State state(argument);
            benchmark.run(state);
            std::cout << name << '\n';
        }
    }
    return 0;
}
