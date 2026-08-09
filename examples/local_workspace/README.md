# Local workspace

This graph contains:

- `demo_app`: the managed root package;
- `demo_math`: a managed local dependency composed into the root build;
- `assets`: a directory without `Kaixa.toml`, represented as opaque and skipped by planning.

```sh
kaixa inspect .
kaixa build .
```

The default CMake dependency mode is `add-subdirectory`. Kaixa injects the dependency project
while configuring `demo_app`, so its `CMakeLists.txt` can link directly to `demo_math`. No install
rules or package configuration files are needed.
