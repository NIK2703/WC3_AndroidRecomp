# Configuration schemas

`config/` holds hand-written inputs and pinned fixtures.  Every top-level JSON
file carries `"schema": 1`; the files here describe that shape.  They are
structural, not semantic: ownership of an import, HLE implementation status and
layout overlap rules are still checked by `tools/hle_coverage.py`,
`tools/check_layout.py` and the C tests.

The validator is `tools/check_config_schema.py` (T0 test `config_schema`).  It
resolves the relative `$ref`s in `common.schema.json` and reports a JSON path
for each mismatch.

Mapping:

| Config | Schema |
|---|---|
| `arity_overrides.1.27b.json` | `arity_overrides.schema.json` |
| `baseline.1.27b.*.json` | `baseline.schema.json` |
| `build_upstream.json` | `build_upstream.schema.json` |
| `cdecl_allowlist.1.27b.json` | `cdecl_allowlist.schema.json` |
| `corpus.1.27b.json` | `corpus.schema.json` |
| `hle_arity.1.27b.json` | `hle_arity.schema.json` |
| `imports.1.27b.json` | `imports.schema.json` |
| `layout.1.27b.json` | `layout.schema.json` |
| `migration.json` | `migration.schema.json` |
| `pe_imports.1.27b.json` | `pe_imports.schema.json` |

`config/import_policy.1.27b.json` is owned by the HLE/M3 track and is not
described here; the validator reports it as absent until that track lands it.
