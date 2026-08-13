# Examples

- [`generated_cmake`](generated_cmake/) generates a CMake project from `Kaixa.toml`.
- [`hello_cmake`](hello_cmake/) adopts an existing CMake project.
- [`local_workspace`](local_workspace/) composes a local dependency with `add_subdirectory`.
- [`package_dependency`](package_dependency/) installs a local dependency and consumes it with `find_package`.

Each example is independent. From an example directory, the complete cycle is:

```sh
kaixa check
kaixa generate
kaixa build
kaixa test
kaixa run
kaixa clean --dry-run
```
