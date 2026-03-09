# FCE — Flat Code Engine

In-memory code graph engine built in C++ with Python bindings.
Parses 10 languages via tree-sitter, builds a symbol graph, and answers queries in microseconds.

| Codebase | Files | Symbols | Index Time |
|---|---|---|---|
| FastAPI | 1,119 | 12,182 | 12.8s |
| Unreal Engine 5.7 | 57,254 | 10,032,665 | 152s |
| Linux Kernel | 63,451 | 7,960,376 | 85s |

<video src="https://github.com/user-attachments/assets/edbbc107-5af0-4299-bc29-7b104e7a308d" controls width="800"></video>

## Why FCE?

Most code intelligence tools rely on external databases (SQLite, LanceDB) and pay for serialization + disk I/O on every query. FCE keeps everything in RAM with a Data-Oriented Design:

- **Zero disk I/O** — SOA arrays in contiguous memory, no DB round-trips
- **Zero serialization** — POD structs used directly, no JSON/protobuf conversion
- **Cache-friendly** — sequential memory layout maximizes L1/L2 prefetch hits
- **Hash lookup ~50ns** vs SQLite SELECT ~50,000ns vs vector search ~500,000ns

## Install

Pre-built wheels are available for **macOS** and **Windows** (Python 3.9–3.13):

```
pip3 install flat-code-engine
```

**Linux** — build from source (requires CMake 3.25+, C++20 compiler):

```bash
pip3 install scikit-build-core pybind11
pip3 install .
```


## Quick Start

```python
import fce

with fce.Engine() as engine:
    engine.index_files(["src/main.cpp", "src/utils.hpp"])
    result = engine.query("MyClass", bfs_depth=2)
    for rel in result.related_symbols:
        print(f"  {rel.relation}: {rel.name} ({rel.kind})")
```

## Python API

### Lifecycle

```python
engine = fce.Engine()
engine.initialize()

# ... use engine ...

engine.release()
```

Context manager is also supported — `initialize()` and `release()` are called automatically.

### Indexing

```python
# Index a list of files
engine.index_files(["src/main.cpp", "src/foo.hpp", "lib/utils.py"])

# Re-index specific files (incremental update)
engine.reindex_files(["src/main.cpp"])

# Remove files from the index
engine.remove_files(["src/old.cpp"])

# Get total symbol count
print(engine.symbol_count())
```

### Query

```python
# Direct lookup — O(1) hash, returns symbols with exact name match
symbols = engine.lookup("MyClass")
for sym in symbols:
    print(f"{sym.kind} {sym.name} @ {sym.file_path}:{sym.start_line}")
    print(f"  parent: {sym.parent_class}")
    print(f"  signature: {sym.signature}")

# BFS traversal — walks the relation graph up to given depth
result = engine.query("MyClass", bfs_depth=2)

# result.symbols: directly matched SymbolEntry list
for sym in result.symbols:
    print(f"{sym.kind} {sym.name}")

# result.related_symbols: RelatedSymbol list discovered via BFS
for rel in result.related_symbols:
    print(f"  {rel.relation} -> {rel.name} ({rel.kind}) depth={rel.depth}")
    print(f"    {rel.file_path}:{rel.start_line}-{rel.end_line}")

# Filter by relation type — only traverse specific edge types
result = engine.query("MyClass", filter=fce.SEMANTIC_RELATIONS)

# Reverse traversal — find symbols that reference this symbol
result = engine.query("MyClass", reverse=True)

# Combine individual relation bitmasks
mask = fce.relation_bit(fce.RelationKind.Inherits) | fce.relation_bit(fce.RelationKind.MemberOf)
result = engine.query("MyClass", filter=mask)
```

### Smart Query (Density-Aware Auto-Expansion)

```python
# Smart query with default settings
result = engine.smart_query("MyClass")

# Custom density configuration
config = fce.DensityConfig()
config.score_threshold = 5.0   # Weighted score threshold
config.min_edge_types = 3      # Minimum distinct edge types
config.max_expansions = 3      # Maximum expansion rounds
config.goldilocks_min = 10     # Minimum related symbols
config.goldilocks_max = 50     # Maximum related symbols

result = engine.smart_query("MyClass", config)

# Density evaluation result
d = result.density
print(f"score={d.weighted_score}, edge_types={d.edge_type_count}")
print(f"sufficient={d.sufficient}, expansions={d.expansions_used}")

# Query result (same structure as QueryResult)
for sym in result.data.symbols:
    print(sym.name)
```

### Introspection

```python
import json

# List all indexed files (JSON)
files = json.loads(engine.get_file_tree())

# List symbols in a specific file (JSON)
symbols = json.loads(engine.get_file_symbols("/path/to/main.cpp"))
for s in symbols:
    print(f"{s['kind']} {s['name']} L{s['start_line']}")

# Search symbols by name prefix (JSON)
matches = json.loads(engine.search_symbols("MyC", limit=20))
```

### Pincer Extract (Code Snippet Extraction)

```python
# Extract code around a specific symbol's call site in a file
snippet = fce.Engine.pincer_extract(
    file_path="src/main.cpp",
    start_line=10,
    end_line=50,
    target_name="process",
    radius=7              # ±7 lines around the call site
)
print(f"{snippet.file_path}:{snippet.start_line}-{snippet.end_line}")
print(snippet.code)
print(f"pincer_applied: {snippet.pincer_applied}")
```

### C Preprocessor (Macro Expansion)

```python
config = fce.PreprocessConfig()
config.enabled = True
config.include_paths = ["/usr/include", "src/"]
config.defines = ["DEBUG=1", "PLATFORM_LINUX"]
config.extra_flags = ["-std=c11"]
config.batch_size = 100    # Files to preprocess per batch

engine.set_preprocess_config(config)
engine.index_files(files)   # Runs gcc -E before parsing
```

### Enums

```python
# SymbolKind
fce.SymbolKind.Function   # Class, Struct, Enum, Variable, Macro,
                          # Alias, Include, Namespace, Concept,
                          # Interface, File, Unknown

# RelationKind
fce.RelationKind.Inherits   # DerivedBy, MemberOf, HasMember,
                            # DefinedIn, Contains, Includes, IncludedBy,
                            # Calls, CalledBy, Returns, ReturnedBy,
                            # TypedAs, TypeOf

# Filter constants
fce.ALL_RELATIONS           # 0 — include all relations
fce.SEMANTIC_RELATIONS      # Excludes structural edges (DefinedIn, Contains, Includes, IncludedBy)
```

## CLI

After `pip3 install flat-code-engine`, the `fce` command is available.

### Interactive REPL

```bash
fce repl src/
```

Index once, then run multiple queries interactively.

```
fce> /query MyClass 3
fce> /lookup main
fce> /search MyC
fce> /symbols src/main.cpp
fce> /opencode MyClass
fce> /smart MyClass
fce> /files
fce> /stats
fce> /quit
```

### One-shot Commands

```bash
# Index files and print stats
fce index src/

# BFS graph traversal by symbol name
fce query MyClass --depth 3 --path src/

# Direct symbol lookup
fce lookup main --path src/

# Search symbols by name prefix
fce search "MyC" --limit 10 --path src/

# List symbols in a specific file
fce symbols src/main.cpp --path src/

# View source code for a symbol
fce opencode MyClass --path src/
```

### C++ (as a static library)

```cmake
add_subdirectory(flat-code-engine)
target_link_libraries(my_app PRIVATE flat-code-engine)
```

```cpp
#include "Core/FceEngine.hpp"

fce::FceEngine engine;
engine.Initialize();
engine.IndexLocalFiles({"src/main.cpp"});

auto result = engine.Query("MyClass", /*bfs_depth=*/2);
for (auto& sym : result.related)
    printf("%s -> %s\n", sym.name.c_str(), sym.filePath.c_str());

engine.Release();
```

## Supported Languages

| Language | Handler | Status |
|---|---|---|
| C | CommonHandlers | Stable |
| C++ | CppHandlers | Stable |
| Python | PythonHandlers | Stable |
| Java | JavaHandlers | Stable |
| JavaScript | JsTsHandlers | Stable |
| TypeScript / TSX | JsTsHandlers | Stable |
| Rust | RustHandlers | Stable |
| Go | GoHandlers | Stable |
| C# | CSharpHandlers | Stable |

## Architecture

```
┌─────────────────────────────────────────────────┐
│                  FceEngine                      │
│  (thread-safe facade, mutex-protected)          │
├─────────────┬───────────────┬───────────────────┤
│ BakeSystem  │  QuerySystem  │ DensityEvaluator  │
│ (indexing)  │  (BFS/lookup) │ (auto-expansion)  │
├─────────────┴───────────────┴───────────────────┤
│              SymbolStore (SOA)                   │
│  CSymbol[]  BakedEdge[]  StringPool             │
├─────────────────────────────────────────────────┤
│         HandlerSet (per-language)                │
│  tree-sitter AST → POD symbols + edges          │
└─────────────────────────────────────────────────┘
```

**Data flow**: Source files → tree-sitter parse → Language handlers extract symbols/edges → SOA arrays in SymbolStore → Hash-based query

### Key design decisions

- **IFceObject lifecycle**: `Initialize()` → `Update()` → `Release()` — game-engine style ownership
- **StringPool interning**: all names stored once, referenced by `uint32_t` ID
- **BakedSymbol / BakedEdge**: fixed-size POD structs, no heap pointers
- **HandlerSet dispatch**: `TSSymbol` → function pointer table, O(1) per AST node
- **Preprocessor**: optional `gcc -E` batch preprocessing for macro expansion

## Relation Graph

FCE tracks 14 relation types between symbols:

| Relation | Inverse | Example |
|---|---|---|
| Inherits | DerivedBy | `class Dog : Animal` |
| MemberOf | HasMember | `Dog::bark` → `Dog` |
| DefinedIn | Contains | `main` → `main.cpp` |
| Includes | IncludedBy | `main.cpp` → `<stdio.h>` |
| Calls | CalledBy | `main` → `printf` |
| Returns | ReturnedBy | `getSize` → `size_t` |
| TypedAs | TypeOf | `count` → `int` |

## Tech Stack

### Core
- **C++20** — structured bindings, `std::string_view`, `/utf-8` (MSVC), `-Wall -Wextra -Wpedantic` (GCC/Clang)
- **Data-Oriented Design** — SOA dense arrays, POD structs (`BakedSymbol` 28B, `BakedEdge` 16B), zero heap allocation in hot paths
- **StringPool** — contiguous `std::vector<char>` buffer, FNV-1a hash, `uint32_t` interning, thread-local pools during parsing

### Parsing
- **[tree-sitter](https://tree-sitter.github.io/)** — 10 language grammars (C, C++, Python, Java, JS, TS/TSX, Rust, Go, C#)
- **Explicit stack DFS** — no recursion, `std::vector<BakeFrame>` work queue, 2M iteration safety limit
- **O(1) handler dispatch** — `HandleFn table[1024]` indexed by `TSSymbol`, no strcmp chains
- **Preprocessor** — `gcc -E` batch mode, temp file bundling (~50x process reduction), linemarker parsing for source mapping

### Threading
- **`std::thread`** worker pool — `hardware_concurrency()` adaptive scaling, chunk distribution
- **`std::atomic<int64_t>`** — lock-free stats accumulation (`memory_order_relaxed`)
- **`std::mutex`** — coarse-grained locking at public API boundary only
- **Thread-local isolation** — each worker owns private StringPool + results vector, zero shared mutable state

### Query
- **Triple hash index** — `nameIndex[nameId]`, `fromIndex[nameId]`, `toIndex[nameId]` for O(1) lookup + BFS
- **BFS** — `std::queue` + `std::set<uint32_t>` visited, 10K node limit, bitmask relation filter
- **Prefix search** — sorted `(lowercase_name, nameId, symIdx)` array, `std::lower_bound()` O(log N)
- **Density evaluator** — 2-gate system (weighted score matrix 12x14 + edge type diversity), auto-expansion up to 3 rounds

### Python Integration
- **[pybind11](https://github.com/pybind/pybind11) v2.13.6** — C++ to Python bindings, context manager support
- **[scikit-build-core](https://github.com/scikit-build/scikit-build-core)** — CMake-based Python package build
- **[cibuildwheel](https://cibuildwheel.readthedocs.io/) v2.21.3** — cross-platform wheel generation (Python 3.9–3.13)

### Build & CI
- **CMake 3.25+** — FetchContent for all dependencies, zero manual downloads
- **[nlohmann/json](https://github.com/nlohmann/json) v3.11.3** — JSON serialization for introspection APIs
- **GitHub Actions** — build matrix (Ubuntu, macOS, Windows), automated testing, PyPI trusted publishing

### Profiling
- **`std::chrono::high_resolution_clock`** — per-stage timing (preprocess, parse, read, remap)
- **`BakeStats`** — atomic counters for preprocessed/raw file counts and millisecond breakdown

## Known Limitations

FCE is under active development. There are likely undiscovered edge cases in parsing and symbol extraction. The C/C++ handlers are the most battle-tested; handlers for other languages (Python, Java, JS/TS, Rust, Go, C#) may have more gaps since they haven't been tested as extensively.

If you find any issues or have suggestions, please [open a GitHub Issue](https://github.com/leejongha45-beep/flat-code-engine/issues). All feedback is greatly appreciated. Thank you.

## License

MIT
