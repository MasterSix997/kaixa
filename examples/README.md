# Examples

`manifest_writer.cpp` is an example associated with the Kaixa package itself. From the repository root it can be listed and run with:

```sh
kaixa run --examples --list
kaixa run --example manifest_writer
```

The remaining directories are independent example projects:

- [`generated_cmake`](generated_cmake/) generates a CMake project from `Kaixa.toml`.
- [`hello_cmake`](hello_cmake/) adopts an existing CMake project.
- [`local_workspace`](local_workspace/) composes a local dependency with `add_subdirectory`.
- [`package_dependency`](package_dependency/) installs a local dependency and consumes it with `find_package`.

From one of those directories, the complete cycle is:

```sh
kaixa check
kaixa generate
kaixa build
kaixa test
kaixa run
kaixa clean --dry-run
```
