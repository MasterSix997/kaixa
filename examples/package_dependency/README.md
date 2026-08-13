# Package dependency

This example keeps two CMake projects isolated. `package_math` is built and installed into the
Kaixa state directory, then `package_app` discovers its exported target through `find_package`.

The consumer selects that integration explicitly:

```toml
[cmake.dependencies]
package_math = "find-package"
```

```sh
kaixa inspect
kaixa generate
kaixa build
kaixa test
kaixa run
```
