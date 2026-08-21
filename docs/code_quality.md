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
kaixa generate --config quality
python tools/quality.py
```

## Checks

- `rules`: repository conventions and invalid text.
- `format`: clang-format on changed C/C++ files.
- `complexity`: Lizard limits.
- `tidy`: compiler-aware diagnostics.

Run selected checks:

```sh
python tools/quality.py rules format complexity
python tools/quality.py tidy --compile-commands .kaixa/build/cmake/clang-debug+quality/kaixa
```

Format changed files:

```sh
python tools/quality.py format --fix
```

Audit all files:

```sh
python tools/quality.py format --all-files
```

## Complexity limits

- CCN: 40
- Lines: 250
- Parameters: 7

Existing exceptions are in `tools/complexity-baseline.txt`.
Remove an exception after fixing its function.
