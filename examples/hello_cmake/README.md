# Existing CMake project

This example adopts a handwritten `CMakeLists.txt`. Kaixa leaves the source project untouched and
keeps its CMake build tree under `.kaixa/build/cmake/<configuration>/hello_cmake`.

```sh
kaixa inspect packages
kaixa generate
kaixa build
kaixa test
kaixa run
```
