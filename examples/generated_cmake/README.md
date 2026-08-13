# Generated CMake

This package has no handwritten `CMakeLists.txt`. Kaixa creates one from the target fields in
`[cmake]`, then configures it as an ordinary CMake project:

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
