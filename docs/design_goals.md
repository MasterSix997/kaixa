`Kaixa` goal is to be a generic package resolver.
> Whether managing everything from scratch, or in conjunction with existing ecosystems (e.g., cmake).
> used both to resolve dependencies, order build, consumed via CLI and embedded (lib) in GUI.

## Principles

- **Core agnostic:** The core resolves dependency graphs and orchestrates passes.
  Don't know about c++, cmake, lua, or asset pipeline. The entire specification lives in resolvers/plugins.
- **Hierarchical:** The dependency tree is declared statically in the manifests and known before any pass runs.
- **Partial adoption:** Folders without manifest (opaque), git submodules without manifest, and managed packages share the same tree.

## Nodes

```
Node
    - Managed:     They have a kaixa manifest, declared resolve, participate in the build
    - Opaque:      Exists in the filesystem, without manifest
    - Submodule:   git submodule; Opaque by default, or managed if it has a manifest inside
```

> To make an opaque node managed, simply add a Kaixa.toml inside the folder

## CLI

1. **Base commands with hook:** `build`, `run`, `test`, `example`, `create`/`new`, `clean`.
   Meaning referring to each resolve, they attach via hooks (pre/pos).
2. **Namespaced commands:** `target:commands` (e.g., `cmake:generate`, `engine:bake_shaders`)

## Reference manifests

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

### Workspace
```toml
[package]
name = "engine"
version = "0.1.0"
resolver = "workspace"

[members]
core = "core"
render = "render"
physics = "physics"

[dev-members]
reflection_prepass = "dev/reflection_prepass"

[commands]
bake_shaders = { script = "scripts/bake_shaders.sh" }
run = { script = "scripts/run_dev", api = true }
```

### Multilingual with subfolders
```toml
[package]
name = "render_2d"
resolver = "workspace"

[members]
core = "core"           # core/kaixa.toml -> resolver = "cmake"
scripting = "scripting" # scripting/kaixa.toml -> resolver = "lua"
fastpath = "fastpath"   # fastpath/kaixa.toml -> resolver = "zig"
```

### Multilingual inline
> This manifest issues three nodes, each with a resolver, without using subfolders.
> A single folder may hold multiple languages: nodes are partitioned by file set,
> not by folder. See *Source partitioning* below.
```toml
[package]
name = "render_2d"
version = "0.1.0"
resolver = "workspace"

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