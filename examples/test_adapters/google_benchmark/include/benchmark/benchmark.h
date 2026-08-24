#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace benchmark {
    class State {
    public:
        struct Iterator {
            bool finished;

            int operator*() const noexcept { return 0; }
            Iterator& operator++() noexcept {
                finished = true;
                return *this;
            }
            bool operator!=(const Iterator& other) const noexcept { return finished != other.finished; }
        };

        explicit State(int argument)
            : m_argument(argument) {}

        [[nodiscard]] Iterator begin() const noexcept { return {false}; }
        [[nodiscard]] Iterator end() const noexcept { return {true}; }
        [[nodiscard]] int range(std::size_t) const noexcept { return m_argument; }

    private:
        int m_argument;
    };

    struct BenchmarkCase {
        std::string name;
        void (*run)(State&);
        std::vector<int> arguments;
    };

    std::vector<BenchmarkCase>& registry();

    class Registrar {
    public:
        Registrar(const char* name, void (*run)(State&));
        Registrar* Arg(int argument);

    private:
        std::size_t m_index;
    };

    Registrar* register_benchmark(const char* name, void (*run)(State&));

    template <typename Value> void do_not_optimize(const Value& value) {
#if defined(_MSC_VER)
        _ReadWriteBarrier();
        static_cast<void>(value);
#else
        asm volatile("" : : "g"(value) : "memory");
#endif
    }
}

#define KAIXA_BENCHMARK_JOIN_INNER(left, right) left##right
#define KAIXA_BENCHMARK_JOIN(left, right) KAIXA_BENCHMARK_JOIN_INNER(left, right)
#define BENCHMARK(function)                                                                                                                \
    static ::benchmark::Registrar* KAIXA_BENCHMARK_JOIN(kaixa_benchmark_, __LINE__) = ::benchmark::register_benchmark(#function, function)
