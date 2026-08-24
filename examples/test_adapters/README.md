# Test adapters

`adapter_example.tests` has no `main()`. Declaring `framework = "googletest"` adds the local
`googletest` package and links its `gtest_main` product. CMake then discovers `Calculator.Adds` and
`Calculator.Multiplies` as separate CTest cases.

`adapter_example.benchmarks` follows the same path through `google_benchmark` and
`benchmark::benchmark_main`. The local framework packages implement the discovery contracts used
by the adapters, keeping the example self-contained.

```sh
kaixa test --list
kaixa test
kaixa test Calculator.Adds
kaixa bench --list
kaixa bench
```
