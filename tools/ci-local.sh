#!/usr/bin/env bash

set -euo pipefail

job="${1:-all}"

run_quality() {
    python3 tools/quality.py rules complexity
    python3 tools/quality.py format --all-files
    cmake --fresh -S . -B build-ci-quality-linux -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    python3 tools/quality.py tidy --compile-commands build-ci-quality-linux
}

run_linux() {
    cmake --fresh -S . -B build-ci-linux \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror -Wno-missing-field-initializers"
    cmake --build build-ci-linux --parallel --target kaixa kaixa_tests
    ctest --test-dir build-ci-linux --output-on-failure --label-regex "^kaixa[.]purpose:test$"
}

git config --global --add safe.directory /workspace

case "$job" in
    quality)
        run_quality
        ;;
    linux)
        run_linux
        ;;
    all)
        run_quality
        run_linux
        ;;
    *)
        echo "usage: tools/ci-local.sh [all|quality|linux]" >&2
        exit 2
        ;;
esac
