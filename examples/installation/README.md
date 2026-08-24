# Installation

`install_example` links the local `runtime_support` library. That dependency publishes a public
header and declares `runtime/runtime.dat` as a runtime file. Building copies the file beside the executable;
installing propagates it with the executable, library, header and CMake package exports.

```sh
kaixa build
.kaixa/build/debug/bin/install_example.exe

kaixa install --prefix .kaixa/stage
.kaixa/stage/bin/install_example.exe
```
