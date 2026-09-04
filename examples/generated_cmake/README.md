# Generated CMake

This package has no handwritten `CMakeLists.txt`. Kaixa creates `CMakeLists.txt` and
`KaixaDependencies.cmake` from `[cmake]`. The generated project builds with plain CMake and installs
a package consumable through `find_package`.

Generation is exportable by default. Set `generation = "state"` in `[cmake]` to keep both generated
files under `.kaixa` instead.

The generated file is marked as Kaixa-owned.
`kaixa clean` preserves it.
`kaixa clean --generated-files` removes it.
Kaixa refuses to overwrite or remove a manual `CMakeLists.txt`.

```sh
kaixa generate
kaixa build
kaixa test
kaixa run
```
