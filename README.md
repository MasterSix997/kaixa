# Kaixa

Kaixa loads local package graphs from `Kaixa.toml` and builds managed packages through their existing CMake projects. Dependencies are currently local paths; downloads and registries are not implemented yet.

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

## Programmatic manifests

`Manifest` is also the in-memory authoring model used by tools.
`format_manifest` returns TOML without touching the filesystem, while `write_manifest_file` writes the same representation accepted by the parser.

```cpp
kaixa::Manifest manifest{"game", "cmake"};
manifest.version = kaixa::Version{"0.1.0"};
manifest.dependencies.emplace_back("math", "packages/math");
manifest.resolver_options = kaixa::Value::table({
    {"source", "project"},
    {"target", kaixa::Value::table({
        {"type", "executable"},
        {"sources", kaixa::Value::array({"src/main.cpp"})},
        {"cxx-standard", 23}
    })}
});

auto written = kaixa::write_manifest_file("Kaixa.toml", manifest);
```

Resolver-specific typed helpers can encode their options into `Value`;

## Build configurations

Named configurations are composable fragments. Published configurations live in the root
`Kaixa.toml`; configurations from dependencies do not control the consuming build:

```toml
[build]
default-configs = ["dev"]

[build.configs.dev]
profile = "debug"

[build.configs.dev.resolvers.cmake]
arguments = ["-DBUILD_TESTING=ON"]
```

Machine-specific configurations can live in the user's config file
or in the ignored `Kaixa.user.toml` beside the root manifest:

```toml
[build]
default-configs = ["clang"]

[build.configs.clang.resolvers.cmake]
generator = "Ninja"
c-compiler = "clang"
cxx-compiler = "clang++"
toolchain = "C:/Toolchains/clang.cmake"
```

The user config is `%APPDATA%/Kaixa/config.toml` on Windows 
and `$XDG_CONFIG_HOME/kaixa/config.toml` (falling back to `~/.config/kaixa/config.toml`) elsewhere.

```sh
kaixa build . --config editor --config clang
```

Raw resolver arguments are an explicit, temporary override. A block ends at the next `--for`:

```sh
kaixa build . --for cmake -G Ninja -DBUILD_TESTING=OFF --for lua --trace
```

Arguments are delivered only to the named resolver. Build options such as `--profile` and
`--config` must appear before the first `--for` block.
The effective profile and resolver settings form part of the CMake build-directory identity,
so configurations using different generators or compilers do not share a `CMakeCache.txt`.

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
