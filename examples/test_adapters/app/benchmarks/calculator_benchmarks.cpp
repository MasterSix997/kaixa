#include <adapter_example/calculator.hpp>

#include <benchmark/benchmark.h>

static void BM_Add(benchmark::State& state) {
    for (const auto iteration: state) {
        static_cast<void>(iteration);
        benchmark::do_not_optimize(adapter_example::add(state.range(0), 2));
    }
}

BENCHMARK(BM_Add)->Arg(8)->Arg(64);

static void BM_Multiply(benchmark::State& state) {
    for (const auto iteration: state) {
        static_cast<void>(iteration);
        benchmark::do_not_optimize(adapter_example::multiply(state.range(0), 2));
    }
}

BENCHMARK(BM_Multiply)->Arg(8);
