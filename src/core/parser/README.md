# TDF Parser

Parses TA:K's TDF (Tagged Data Format) files — the INI-like config format used for unit stats, campaign missions, weapon data, and more.

## Architecture

The parser builds a **tree** of `TDFEntry` nodes (defined internally in `tdf_parser.c`). The public API (`tak_tdf.h`) exposes an opaque `TDFFile` handle with stack-based cursor navigation:

```
TDF_Open("unit.tdf")     →  read file into memory
TDF_Load(tdf)            →  parse raw text into tree
TDF_PushSection("WEAPON1")  →  descend into section
TDF_ReadInt("range", 0)     →  read key from current section
TDF_PopSection()            →  return to parent
TDF_Close(tdf)              →  free everything
```

Nested sections (e.g. `[DAMAGE]` inside `[WEAPON1]`) are first-class tree children, not flattened.

## Files

| File | Purpose |
|------|---------|
| `tdf_parser.c` | Main implementation — tree structures, file I/O, navigation, value readers |
| `tdf_parser.h` | Old flat API header (deprecated — use `tak_tdf.h` instead) |
| `tdf_parser-v1.c` | Early draft (archived, not compiled) |
| `tak_memory_stub.c` | Stdlib shims for `tak_malloc`/`tak_free` — lets tests link without full `memory.c` |
| `test_tdf_parser.c` | Unit tests for the tree-based API |
| `test.tdf` | Sample TDF file with nested sections for reference |

## Running Tests

From the project root:

```bash
# Configure (once)
cmake -B build

# Build tests
cmake --build build --target test_tdf_parser --config Debug

# Run
./build/src/Debug/test_tdf_parser.exe
```

## Test Status

`tdf_parse_string()` (called by `TDF_Load`) is **not yet implemented**. Tests that depend on parsing will fail until it's done. Currently passing:

- `TDF_Open` / `TDF_Close` lifecycle (open files, handle NULL, no leaks)
- Error detection (unclosed sections, missing braces)

Tests waiting on `tdf_parse_string`:

- Section parsing (single, multiple, nested subsections)
- Key-value reading (strings, ints, floats, defaults)
- Whitespace/comment handling
- Navigation (PushSection, PopSection, OpenSection by index)

## Test Framework

Tests use `include/test_framework.h` — a lightweight header-only test harness shared across the project. See that file for available macros (`TEST`, `RUN`, `ASSERT_EQ_INT`, `ASSERT_EQ_STR`, etc.).
