# Kaixa

The MVP loads local package graphs from `Kaixa.toml` and builds managed packages through their
existing CMake projects. Dependencies are local paths; downloads and registries are intentionally
out of scope.

```toml
[package]
name = "app"
version = "0.1.0"
resolver = "cmake"

[dependencies]
math = { path = "libs/math" }
```

```sh
kaixa inspect .
kaixa build .
kaixa build . --profile release
```

## Build Kaixa

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```
