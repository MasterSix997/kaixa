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

CMake dependencies are composed from source with `add_subdirectory` by default. A consumer that
uses a dependency as an exported CMake package can opt into the isolated `install` + `find_package`
flow:

```toml
[cmake.dependencies]
math = "find-package"
```

Kaixa keeps resolver-owned build files and package artifacts under `.kaixa/`; these paths are not
part of the core resolver contract.

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
