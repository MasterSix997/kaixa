# Local workspace

This graph contains:

- `demo_app`: the managed root package;
- `demo_math`: a managed local dependency, built before the root;
- `assets`: a directory without `Kaixa.toml`, represented as opaque and skipped by planning.

```sh
kaixa inspect .
kaixa build .
```