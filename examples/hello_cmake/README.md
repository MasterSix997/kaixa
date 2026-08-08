# Hello CMake

A minimal managed package. Kaixa reads `Kaixa.toml`, delegates planning to the CMake resolver and
places the external build under `.kaixa/build/hello_cmake`.

```sh
kaixa inspect .
kaixa build .
```
