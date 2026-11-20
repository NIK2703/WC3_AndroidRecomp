# Patch pipeline registry

`tools/lift/patch_pipeline.py` is the general build-time patch layer for the
lifted corpus.  A stage is data, not Python code: the orchestrator knows how to
run per-chunk, batch and one-shot stages, but it does not know any patch name.

The lifted corpus is immutable.  The pipeline materializes a private work tree,
runs the selected stages in dependency order, verifies their reports, and writes
an overlay directory with content hashes and provenance.  The overlay is the
only build product of the patch layer.

## Entry points

```sh
# One-time (or cached) function index.
python3 tools/lift/patch_pipeline.py index \
    --base build/corpus-full/game --out build/patch-index.tsv

# Resolve the graph without running an engine.
python3 tools/lift/patch_pipeline.py check \
    --index build/patch-index.tsv \
    --registry tools/lift/patches/registry.json \
    --patch w3l_compat --param profile=rubattle

# Apply selected stages and write the overlay.
python3 tools/lift/patch_pipeline.py apply \
    --base build/corpus-full/game \
    --index build/patch-index.tsv \
    --registry tools/lift/patches/registry.json \
    --patch w3l_compat --patch selection \
    --param profile=rubattle --out build/patch-overlay

# Run a named pipeline from the registry.
python3 tools/lift/patch_pipeline.py apply \
    --base build/corpus-full/game \
    --index build/patch-index.tsv \
    --pipeline post_save --out build/patch-overlay

# Inspect the registry.
python3 tools/lift/patch_pipeline.py list
python3 tools/lift/patch_pipeline.py graph
```

`patch_overlay.py` is a compatibility wrapper for the same CLI.

## Registry

`registry.json` is the single project entry point.  It lists stage manifests
and reserved future stages:

```json
{
  "schema": 2,
  "id": "wc3-build-patches",
  "stages": [
    "atomiccheck.json",
    "save_list_display.json",
    {"manifest": "return_address_elide_index.json", "enabled": false},
    {"manifest": "devirtualize.json", "enabled": false},
    {"manifest": "return_address_elide.json", "enabled": false},
    {"manifest": "dead_flags.json", "enabled": false},
    {"manifest": "xmm_local.json", "enabled": false},
    {"manifest": "xmm_lane.json", "enabled": false},
    "w3l_compat.json",
    "selection.json",
    "leaf_call.json",
    "parallel_raycast.json"
  ],
  "pipelines": {
    "semantic": ["atomiccheck", "w3l_compat", "selection",
                 "leaf_call", "parallel_raycast"],
    "post_save": ["atomiccheck", "return_address_elide_index",
                  "devirtualize", "return_address_elide", "dead_flags",
                  "xmm_local", "xmm_lane", "w3l_compat",
                  "selection", "leaf_call", "parallel_raycast"],
    "raw": ["atomiccheck", "save_list_display", "return_address_elide_index",
            "devirtualize", "return_address_elide", "dead_flags",
            "xmm_local", "xmm_lane", "w3l_compat", "selection",
            "leaf_call", "parallel_raycast"]
  },
  "reserved": [
    {"id": "guest_data", "kind": "data", "reason": "guest-memory data overlay"},
    {"id": "link_wrappers", "kind": "link", "reason": "link-time wrapper map"}
  ]
}
```

`semantic` is the subset that can run on a base already containing the
LiftPass stages.  `post_save` is the full canonical chain for the project's
split base (save already applied).  `raw` includes the save stage and is for a
pre-save split base.  A pipeline is just a list of stage IDs; the DAG still
orders them and rejects cycles.

A stage manifest is a JSON object with `schema: 2` and:

| Field | Meaning |
|---|---|
| `id` | Stable stage identity. |
| `engine` | Executable name in `build/lift-tools` (`.py` is allowed for tests). |
| `kind` | `transform`, `gate`, `prepare`, `data`, `link`. |
| `mode` | `per_chunk`, `batch`, `once`. |
| `scope` | `targets`, `all`, `once`. |
| `params` | Optional default values for placeholders; CLI `--param` wins. |
| `order` | Tie-break inside the dependency graph. |
| `after` | Stage IDs that must run first when both are selected. |
| `targets` | Function-level targets for `scope: targets`. |
| `expect_sites` | Integer or per-profile map for target coverage. |
| `command.args` | Engine arguments with placeholders. |
| `command.first_chunk_args` | Extra arguments for the first target chunk. |
| `command.inputs_args` | Once-only flags repeated for every input path. |
| `command.outputs` | Files a `prepare` stage must create. |
| `assertions` | Generic report/text assertions. |
| `coverage` | Optional engine coverage check. |
| `output_layout` | `preserve` or `flat` for batch output. |

`after` is a partial order.  If a dependency is not selected, the stage is
allowed to run against a base that already contains it; the dependency is
reported as an external dependency in `overlay.json`.

## Modes and scopes

- `per_chunk`: one engine call per selected chunk.  Placeholders `{in}`,
  `{out}`, `{report}`, `{sites}`, `{chunk}`, `{state}` and `{root}` are
  available.
- `batch`: one engine call per stage.  Placeholders `{inputs}` (a text file with
  one input path per line), `{out_dir}` and `{report}` are available.  The
  engine must write every selected input under `{out_dir}`, using either the
  preserved relative path or a unique basename (`output_layout: flat`).
- `once`: one engine call after the chunk stages.  Used by `gate` and
  `prepare` stages; `data` and `link` are reserved for guest-data and link-map
  outputs.  A `prepare` stage writes files under `{state}` and declares them in
  `command.outputs`; later stages read them through the same `{state}` path.
  `command.inputs_args` repeats a flag before every input path, which is how a
  whole-corpus index such as `return_address_elide` is built once.

`scope: targets` resolves target functions to exactly one chunk.  `scope: all`
runs over every chunk in the index.  `scope: once` has no chunk set.  A target
may override the stage `module` and `kind`:

```json
{"function": "L_6F2A950C", "sites": 1,
 "module": "game", "kind": "closed"}
```

Ambiguous or missing functions are fatal.  The pipeline never guesses a chunk.

## Assertions

Assertions are evaluated against the engine report or the final C text.  The
currently supported forms are:

```json
{"kind": "report_pass"}
{"kind": "field_equals", "path": "verdict", "equals": "pass"}
{"kind": "field_equals", "path": "inputs", "equals_chunk_count": true}
{"kind": "field_equals", "path": "profile", "equals_param": "profile"}
{"kind": "site_count", "path": "files[0].sites", "equals_targets": true}
{"kind": "function_coverage", "path": "files[0].functions"}
{"kind": "status_coverage", "path": "files[0].sites",
 "function": "function", "status": "status",
 "applied": ["applied", "already"], "absent": "absent"}
{"kind": "counts_sum", "path": "counts", "equals_targets": true}
{"kind": "array_length", "path": "sites", "equals_targets": true}
{"kind": "output_changed"}
{"kind": "marker_count", "string": "...", "equals": 4}
{"kind": "marker_defined_once", "symbol": "..."}
```

Report paths use dotted keys and array indices, for example
`files[0].sites`.  New engines do not need a new Python branch: add the
assertions that describe their report.

`coverage` may be a list of arguments (report paths are appended) or:

```json
{"args": ["--verify-coverage"], "reports": "manifest"}
```

The manifest form writes a text file with one report path per line and passes
`@file`, which is the contract used by `selection`.

## Current stages

| Stage | Mode | Scope | Purpose |
|---|---|---|---|
| `atomiccheck` | once | once | Gate: atomic lowering over every input. |
| `save_list_display` | per_chunk | targets | Declared save-list display delta. |
| `return_address_elide_index` | once | once | Prepare: global P0-6 safety map. |
| `devirtualize` | per_chunk | all | P0-1 indirect-call devirtualization. |
| `return_address_elide` | per_chunk | all | P0-6 return-address store elision. |
| `dead_flags` | per_chunk | all | P0-2 dead flag-write elimination. |
| `xmm_local` | per_chunk | all | Block/lazy local-XMM lowering. |
| `xmm_lane` | per_chunk | all | Upper-lane zero reuse. |
| `w3l_compat` | per_chunk | targets | Declared W3L compatibility profile. |
| `selection` | per_chunk | all | 12/24/40 selection delta; union coverage. |
| `leaf_call` | batch | all | Direct-call rewriting over the whole corpus. |
| `parallel_raycast` | batch | all | Parallel-raycast rewriting after `leaf_call`. |

`xmm_local`, `xmm_lane`, `devirtualize`, `return_address_elide` and
`dead_flags` are present but disabled by default because the current build base
may already contain them.  `save_list_display` is disabled for the same reason:
the project's split base is already post-save.  Select the stages explicitly, or
use the `raw` pipeline for a pre-save split base.  Guest-data and link-map
stages remain reserved.

## Proof status

The synthetic self-test covers per-chunk, batch, ordering, target resolution
and marker assertions.  The real pipeline has been compared byte-for-byte with
the existing stages on the full 263-chunk corpus:

- `w3l_compat` (10 chunks, 12 sites), `save_list_display` (2 chunks);
- the `selection` transform (348 sites over the full corpus; its
  `--verify-coverage` union check is the site-count gate);
- `leaf_call` and `parallel_raycast` batch outputs (263 chunks);
- `devirtualize` + `return_address_elide_index` + `return_address_elide`
  (263 chunks, global index identical);
- `dead_flags` + `xmm_local` + `xmm_lane` (263 chunks);
- the whole 11-stage chain from split corpus to `parallel_raycast` output
  (263 chunks, exact).

## Build integration

`WC3_PATCH_PIPELINE` is an opt-in CMake switch, default `OFF`
(`-DWC3_PATCH_PIPELINE=ON`).  With it on,
`cmake/Wc3ReliftedCorpus.cmake` keeps `splitcorpus` and the pre-split save pass,
skips the old per-chunk and post-loop transform ladder, and runs:

1. `patch_pipeline.py index` over `build/<variant>/cmake/relifted`.
2. `patch_pipeline.py apply` with stages selected from the existing CMake flags
   (`devirtualize`, `return_address_elide`, `dead_flags`, `xmm_local`,
   `xmm_lane`, `w3l_compat`, `selection`, `leaf_call`,
   `parallel_raycast`).
3. The chunk list from `patch-overlay/overlay.json` becomes
   `WC3_RELIFTED_SOURCES`.

The base is the post-save split tree, so the save stage is not selected again.
`apply --reuse --force` validates an existing overlay against base chunk hashes,
index hash, selected stage IDs, manifest/engine hashes and effective params;
a stale overlay is rebuilt.  CMake passes the orchestrator hash as a cache
parameter, so editing `patch_pipeline.py` also invalidates an overlay.
`selection` scans every chunk, like the legacy per-stage path; the
engine's `--verify-coverage` union check against its fixed total is the single
site-count authority, so the manifest carries no duplicated target list.  Its
anchors expect the post-XMM `WC3_DIRECT_CALL` text, so the manifest is ordered
after `xmm_local`, `xmm_lane` and `w3l_compat`; a base that already contains
those passes may omit them.  The stage always emits the fixed 40-entry
layout; the runtime cap selects 12, 24 or 40 without a second corpus.

The switch is not exercised by a configure or product build yet.  The old path
remains the default until that check is run explicitly; the pipeline itself is
proven byte-for-byte on the full 263-chunk corpus (see above).

## Provenance

`overlay.json` records the registry hash, stage hashes, engine hashes,
per-chunk base and overlay hashes, target counts, external dependencies and
coverage status.  `resolve.json` records the resolved stage/chunk/target plan.
Changing a manifest, engine or parameter changes `patchset_sha256`; changing a
base chunk changes its input hash.  The lifted corpus itself is never edited.

## Adding a stage

1. Add the engine manifest under `tools/lift/patches/`.
2. Add the manifest to `registry.json` in dependency order.
3. Run `patch_pipeline.py check` against a real index.
4. Run `patch_pipeline.py apply` on a representative base and compare with the
   current stage output.
5. Add a self-test only if the stage introduces a new execution mode or
   assertion shape.

No stage name belongs in `patch_pipeline.py`.
