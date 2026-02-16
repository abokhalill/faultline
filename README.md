# Faultline

Structural latency landmine detector for ultra-low-latency C++ systems.

Built on Clang/LLVM. Targets x86-64 / 64B cache lines / TSO memory model.

## Prerequisites

- LLVM/Clang 16+ (development libraries)
- CMake 3.20+
- C++20 compiler

### Ubuntu/Debian

```bash
apt install llvm-dev libclang-dev clang cmake
```

### Arch

```bash
pacman -S llvm clang cmake
```

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

If LLVM is installed in a non-standard location:

```bash
cmake .. -DCMAKE_PREFIX_PATH=/path/to/llvm
```

## Usage

```bash
# Analyze a single file (requires compile_commands.json or -- flags)
./faultline /path/to/source.cpp -- -std=c++20

# With config
./faultline --config=../faultline.config.yaml /path/to/source.cpp --

# JSON output
./faultline --json /path/to/source.cpp --

# Filter by severity
./faultline --min-severity=High /path/to/source.cpp --

# With compilation database
cd /project/build
/path/to/faultline /project/src/hot_path.cpp
```

## Hot Path Annotation

Mark functions as latency-critical:

```cpp
[[clang::annotate("faultline_hot")]]
void onMarketData(const MDUpdate& update) { ... }
```

Or use config patterns in `faultline.config.yaml`:

```yaml
hot_function_patterns:
  - "*::onMarketData*"
  - "*::process*"
hot_file_patterns:
  - "*/hot_path/*"
```

## Exit Codes

| Code | Meaning |
|------|---------|
| 0    | No hazards detected |
| 1    | Hazards detected |
| 2    | Tool error |

## Test

```bash
./faultline ../test/samples/fl001_test.cpp -- -std=c++20
```

Expected: `LargeOrder` flagged High, `AtomicHeavy` flagged Critical, `SmallOrder` clean.

## Project Structure

```
include/faultline/
  core/           Severity, Diagnostic, Rule, RuleRegistry, Config, HotPathOracle
  analysis/       FaultlineAction, ASTConsumer, StructLayoutVisitor
  output/         CLI and JSON formatters
src/
  core/           Core implementations
  analysis/       AST traversal and consumer
  output/         Output formatting
  rules/          Individual rule implementations (FL001, ...)
  main.cpp        CLI driver
test/samples/     Test input files
```

## Documentation

- `VISION.md` — Scope and mission
- `ARCHITECTURE.md` — Analysis layers and latency model
- `RULEBOOK.md` — Rule specifications
- `SCORING_MODEL.md` — Validation and benchmarking strategy
- `ROADMAP_AND_LIMITATIONS.md` — Phased delivery and hard limits
