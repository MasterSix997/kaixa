# Code quality

Requirements:

- Python 3
- LLVM 22 on `PATH`

Install Lizard:

```sh
python -m pip install -r tools/quality-requirements.txt
```

Run all checks:

```sh
kaixa task quality
kaixa task tidy --config quality
```

## Checks

- `rules`: repository conventions and invalid text.
- `format`: clang-format on changed C/C++ files.
- `complexity`: Lizard limits.
- `tidy`: compiler-aware diagnostics.

Run selected checks:

```sh
kaixa task quality
kaixa task tidy --config quality
```

Format changed files:

```sh
kaixa task format-check -- --fix
```

Audit all files:

```sh
kaixa task format-check
```

## Complexity limits

- CCN: 40
- Lines: 250
- Parameters: 7

Existing exceptions are in `tools/complexity-baseline.txt`.
Remove an exception after fixing its function.
