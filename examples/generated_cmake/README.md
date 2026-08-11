# Generated CMake

This package has no handwritten `CMakeLists.txt`. Running the build creates one from the
target fields in the `[cmake]` section of `Kaixa.toml`, then configures and builds it normally:

```sh
kaixa build .
```

The generated file is marked as Kaixa-owned. Kaixa refuses to replace an existing manual
`CMakeLists.txt`.
