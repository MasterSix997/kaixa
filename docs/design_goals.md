`Kaixa` goal is to be a generic package resolver.
> Whether managing everything from scratch, or in conjunction with existing ecosystems (e.g., cmake).
> used both to resolve dependencies, order build, consumed via CLI and embedded (lib) in GUI.

## Principles

- **Core agnostic:** The core resolves dependency graphs and orchestrates passes.
  Don't know about c++, cmake, lua, or asset pipeline. The entire specification lives in resolvers/plugins.
- **Hierarchical:** The dependency tree is declared statically in the manifests and known before any pass runs.
- **Partial adoption:** Authored packages, provider-adopted sources and opaque directories share the same tree.
- **Resolver authority:** Adopting an existing project does not require reproducing its build graph in the manifest. The resolver remains authoritative for its internal units and maps observable products back to packages.

## Nodes

```
Node
    - Managed:     Has an authored Kaixa.toml and participates in the build
    - Adopted:     Has no Kaixa.toml; a provider descriptor supplies resolver and consumption semantics
    - Opaque:      Has no build description and remains visible in the dependency graph
    - Submodule:   Opaque by default, managed with Kaixa.toml, or adopted by a provider descriptor
```

> To make an opaque node managed, simply add a Kaixa.toml inside the folder
> To leave an upstream tree untouched but build it, describe its source and consumer in a provider.

## CLI

1. **Base commands with hook:** `build`, `run`, `test`, `example`, `create`/`new`, `clean`.
   Meaning referring to each resolve, they attach via hooks (pre/pos).
2. **Namespaced commands:** `target:commands` (e.g., `cmake:generate`, `component:bake_shaders`)

## Reference manifests

One package per `Kaixa.toml` is the primary layout. Inline members remain available when multiple small packages genuinely benefit from sharing one manifest, but they are not the default organization.

`Kaixa.toml` is reserved for packages and package sets. Associated-target fragments use
`Kaixa.test.toml`, `Kaixa.example.toml` or `Kaixa.benchmark.toml` and inherit their owning package through an explicit target reference.

### Leaf minimum
```toml
[package]
name = "math"
version = "0.1.0"
resolver = "kaixa-native"
```

### Cmake
```toml
[package]
name = "render"
version = "0.1.0"
resolver = "cmake"

[members]
render_3d = "libs/render_3d"
render_ui = "libs/render_ui"

[dependencies]
dx11 = "*"
vulkan = { version = ">=1.3", features = ["validation"]}
slang = { url = "https://github.com/shader-slang/slang/releases/v2024.1.tar.gz"}

[cmake]
minimal_required = "3.20"
add_subdirectory = ["tests"]

# passes are a later phase (BuildGraph); shown here for direction only.
[[pass]]
tool = "slang"
inputs = "render_3d/**/*.slang"
outputs = "generated/shaders"
before = "cmake:build"
```

### Package set
```toml
[package-set]
name = "component"
members = ["core", "render", "physics"]
development-members = ["dev/reflection_prepass"]
```

### Multilingual with subfolders
```toml
[package-set]
name = "render_2d"
members = [
    "core",      # core/Kaixa.toml -> resolver = "cmake"
    "scripting", # scripting/Kaixa.toml -> resolver = "lua"
    "fastpath",  # fastpath/Kaixa.toml -> resolver = "zig"
]
```

### Multilingual inline
> This manifest issues three nodes, each with a resolver, without using subfolders.
> A single folder may hold multiple languages: nodes are partitioned by file set,
> not by folder. See *Source partitioning* below.
```toml
[package-set]
name = "render_2d"

[members.core]
resolver = "cmake"
sources = ["src/**/*.{cpp,hpp}", "include"]
cmake.minimal_required = "3.20"

[members.scripting]
resolver = "lua"
sources = ["src/**/*.lua"]

[members.fastpath]
resolver = "zig"
sources = ["src/**/*.zig"]
```

## Source partitioning

Inline members share the filesystem, so the boundary between two nodes is the
**file set**, not the folder. The rule: **no file may belong to two resolvers.**
Overlapping `sources` is an error the core rejects up front.

Two ways to stay disjoint:

- **By extension (default).** A resolver declares which extensions it claims
  (`cmake` -> `.cpp/.hpp/.c`, `zig` -> `.zig`, `lua` -> `.lua`).
  Members can then point at the same folder (`sources = ["src"]`) and the core routes each file to
  the resolver that claims its extension.
  A file matching two resolvers is the only conflict, and it is rare and explicit.
- **By glob (override).** Explicit `sources` globs when extension is not enough
  (e.g. two groups of `.cpp` feeding different targets).

A file genuinely needed by two resolvers is not co-owned source: it is an
*output* one pass produces and another consumes, i.e. a graph edge, not a second owner.
