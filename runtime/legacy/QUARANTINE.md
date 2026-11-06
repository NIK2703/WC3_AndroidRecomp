# Migration quarantine (R15)

These files are the as-built HLE/guest sources imported from the read-only
legacy tree for the first executable product milestone. They are inputs, not
the canonical implementation: `runtime/legacy` is quarantined, target code
reaches it only through `runtime/hle/legacy_shim` (H6.3), and every file has a
target module, owner step and expiry.

`hle_android.c` carries one local Android null-D3D9 fix (OP_LOCK_BUFFER returns
a writable guest scratch buffer) that was made before the quarantine rule was
ratified. Its `sha256` below is the current file; the source path remains the
legacy origin. The fix belongs to the D3D9/HLE module and is not a license to
edit other quarantined files.

| file | source_path | source_revision | sha256 | target_module | owner_step | expires_at |
|---|---|---|---|---|---|---|
| `boot_probe.c` | `E:/projects/warcraft3_recomp/runtime/android/boot_probe.c` | legacy working tree 2026-11-06 | `0272edf97572f736c67f88749a47b1bb90d056d43fa332aa3a8fd8e751986ce2` | `boot` | M3 | 2027-06-30 |
| `hle_android.c` | `E:/projects/warcraft3_recomp/runtime/android/hle_android.c` | legacy working tree + null-D3D9 Lock patch | `382845d0efa03b464911b3577ba81460f8d194b8115008fcffa0ba0e83466a3c` | `hle` | M5 | 2027-06-30 |
| `hle_android.h` | `E:/projects/warcraft3_recomp/runtime/android/hle_android.h` | legacy working tree 2026-11-06 | `f4c3e07cf07eca442d9bb5b1313e3a738388260b157055695049390f3bcf0f41` | `hle` | M5 | 2027-06-30 |
| `hle_input_state.h` | `E:/projects/warcraft3_recomp/runtime/android/hle_input_state.h` | legacy working tree 2026-11-06 | `a2aecd10978975de2b429b48376e680dbdc051a1e43bb9cd822b9bb0adeacdce` | `hle` | M5 | 2027-06-30 |
| `hle_registry.h` | `E:/projects/warcraft3_recomp/runtime/android/hle_registry.h` | legacy working tree 2026-11-06 | `d78dd18c70fd2fbc8541d6eec654676ed715d0b5992a479c0d23f92bb5d5a69c` | `hle` | M5 | 2027-06-30 |
| `host.h` | `E:/projects/warcraft3_recomp/runtime/host/host.h` | legacy working tree 2026-11-06 | `fc5ed0ef15687dca928f7c1842902fafde7ba6933e6d7e765c06993ce9c237a5` | `guest` | M3 | 2027-06-30 |
| `pe_format.h` | `E:/projects/warcraft3_recomp/runtime/host/pe_format.h` | legacy working tree 2026-11-06 | `e2be97725c86f9e870b0fe9006877d1b01952bc0193e391212db7f75594fdf1f` | `guest` | M3 | 2027-06-30 |
| `guest_layout.h` | `E:/projects/warcraft3_recomp/runtime/host/guest_layout.h` | legacy working tree 2026-11-06 | `765aa955253efa84680fb1a3e5696221cd264e3c3ba5421ce0ee713abc1dcb66` | `guest` | M3 | 2027-06-30 |
| `pe32.c` | `E:/projects/warcraft3_recomp/runtime/host/pe32.c` | legacy working tree 2026-11-06 | `9641a3302c5a62f598bb2c4fe87ad97e04d72a5844055e910f092a7e1cd93931` | `guest` | M3 | 2027-06-30 |
| `guest_mem.c` | `E:/projects/warcraft3_recomp/runtime/host/guest_mem.c` | legacy working tree 2026-11-06 | `f5803dad296082c40a9c451c0c70905e9112b144d6fbedf400865a7e75702e8f` | `guest` | M3 | 2027-06-30 |
| `d3d9_tables.h` | `E:/projects/warcraft3_recomp/runtime/host/d3d9_tables.h` | legacy working tree 2026-11-06 | `705cfed566639e2744d60a845199a1fbbc0afc2b5b9fc83623b45fcdb4bfe4d1` | `hle-d3d9` | M5 | 2027-06-30 |
| `hle_widescreen127.c` | `E:/projects/warcraft3_recomp/runtime/host/hle_widescreen127.c` | legacy working tree 2026-11-06 | `3b38f685dcd8bc50ede376ebccce284b88432a696c89070880dead4c6dfea42d` | `hle` | M5 | 2027-06-30 |
