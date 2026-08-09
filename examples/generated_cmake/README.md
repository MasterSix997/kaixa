# Generated CMake

This package has no handwritten `CMakeLists.txt`. Running the build creates one from the
`[cmake.target]` section in `Kaixa.toml`, then configures and builds it normally:

```sh
kaixa build .
```

The generated file is marked as Kaixa-owned. Kaixa refuses to replace an existing manual
`CMakeLists.txt`.
