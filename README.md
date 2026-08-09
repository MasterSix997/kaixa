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

## Generated CMake project

When `[cmake.target]` is present, Kaixa generates the package's `CMakeLists.txt`.
Supports one target named after the package:

```toml
[package]
name = "hello"
resolver = "cmake"

[cmake]
languages = ["CXX"]

[cmake.target]
type = "executable"
sources = ["src/main.cpp"]
include-directories = ["include"]
link-libraries = ["some_private_target"]
public-include-directories = []
public-link-libraries = []
cxx-standard = 23
```

Supported target types are `executable`, `static-library`, `shared-library` and `interface-library`.
Kaixa updates only a `CMakeLists.txt` carrying its generated-file marker;
it refuses to overwrite a manually maintained file.

Generator, compilers, toolchain and other machine-specific settings are build inputs.
Pass native CMake configure arguments after `--`:

```sh
kaixa build . -- -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain.cmake -DBUILD_TESTING=OFF
```

These arguments apply to every CMake build unit in the requested workspace.
Generator, compiler and toolchain are cached by CMake; after changing them, remove the corresponding package directory
under `.kaixa/build/cmake/` before rebuilding.

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
