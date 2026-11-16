# Distributed C/C++ compilation on IDEAPAD

Accelerator for the canonical Android build. Every native variant
(`release`, `debug`, `release_optimized`, `release_optimized_pgo`) tries
IDEAPAD automatically and falls back to a plain local build when the peer is
unreachable. `build_dist.ps1` remains the explicit entry point for transport,
capacity and diagnostic overrides. `ccache` remains the compiler launcher. On
a cache miss `wc3_dist_cc.py` either hands the command to a remote `distccd`
through a persistent TCP-over-SSH tunnel (default), or uses the original SSH
helper protocol in `compile_one.py`.

The remote worker uses the same NDK at the same absolute path, so the object
layout is the same as a local compile. The local dependency file, macros and
`__FILE__` expansion still come from the local preprocessor.

## Capacity model

The scripts read the capacity of both machines instead of using a fixed split:

| Machine | Source of slot count | Typical count |
| --- | --- | --- |
| this PC | `[Environment]::ProcessorCount` | 4 |
| IDEAPAD | remote `NUMBER_OF_PROCESSORS` | 12 |
| Ninja | local + remote slots | 16 |

`build_dist.ps1` writes:

```
DISTCC_HOSTS=127.0.0.1:<free-port>/12 localhost/4 --localslots=4 --localslots_cpp=4
CMAKE_BUILD_PARALLEL_LEVEL=16
```

distcc tries the remote host first. The first 12 compiles go to IDEAPAD; the
next four use the local four slots. In steady state a free remote slot is
always preferred, so the faster machine receives proportionally more work. The
local compiler concurrency is capped at four by the `localhost/4` slots, even
though Ninja has 16 jobs in flight.

Commands that cannot be distributed for identity reasons (debug info,
modules, response files, non-clang compilers, unsupported profile modes) are
sent through distcc with a localhost-only host list. They therefore share the
same four local slots. The custom `ssh` transport uses a counted local-slot
file semaphore for the same cap.

ThinLTO and PGO commands are distributable only when the automatic helper or
`build_dist.ps1` enables them for an optimized variant. The eligibility rules
are explicit: `-flto*` needs `WC3_DIST_ALLOW_LTO=1`; profile-generate needs
`WC3_DIST_ALLOW_PGO=1`; profile-use additionally needs
`WC3_DIST_PGO_PROFILE_READY=1`. A final `release_optimized` build copies its
merged profile to the same absolute path on IDEAPAD before the first compile,
so the local preprocessor and the remote compiler both see a real profile.
Without that copy, profile-use commands stay local instead of failing.

`-LocalSlots`, `-RemoteSlots` and `-Jobs` override the detected values.
The default `Jobs` is `local + remote` for every native variant. Debug corpus
objects end in `-g0` and are distributable; commands whose final state keeps
debug info use the localhost-only distcc host list and stay capped by
`LocalSlots`. `-Jobs` only changes how many commands Ninja starts; it does not
remove the distcc local slot limit. Leaving it below
`LocalSlots + RemoteSlots` leaves remote capacity idle.

A ccache hit does not enter distcc at all; it is cheap and finishes without a
slot. ccache itself still runs one local preprocessor per miss, so a burst of
16 misses briefly adds local preprocessing load beyond the four compile slots.
That is the main reason small translation units are not faster when
distributed.

## Prerequisites

IDEAPAD needs:

* the same NDK at `E:\Android\Sdk\ndk\29.0.14206865`;
* the self-built distcc 3.4 binaries in `E:\wc3-dist\bin` (`distccd.exe`,
  `msys-2.0.dll`);
* the helper script `E:\wc3-dist\compile_one.py` for the `ssh` transport;
* an SSH server reachable through the existing `id_ideapad` key and `nikita`
  account.

This machine needs `C:\msys64\usr\bin\distcc.exe`. That location is deliberate:
ccache fails to launch the client with exit 127 from `.local\bin` and temporary
directories, while the MSYS2 `usr\bin` path works through `CCACHE_PREFIX`.

The peer is resolved through the local DNS name `ideapad`; the current IPv4
neighbor entry is then matched to its IPv6 link-local address and interface
index. No address, suffix or interface index is stored in the scripts, so a
new DHCP lease or link-local change is picked up on the next start.
`WC3_DIST_HOST` overrides the resolver.

## Usage

```powershell
# normal release build: IDEAPAD is used when reachable, otherwise local
.\tools\android\build_app.ps1 -Variant release
.\tools\android\build_apk.ps1 -Release

# force a local build for one invocation
$env:WC3_DIST_AUTO = '0'
.\tools\android\build_app.ps1 -Variant release

# explicit transport entry point (distcc is the default)
.\tools\dist\build_dist.ps1 -Variant release

# final optimized build: copy the merged profile before compiling
.\tools\dist\build_dist.ps1 -Variant release_optimized `
    -PgoProfile build/pgo/wc3.profdata

# validate resolution, tunnel and environment without starting a build
.\tools\dist\build_dist.ps1 -Variant release -DryRun

# use the original SSH helper transport instead
.\tools\dist\build_dist.ps1 -Variant release -Transport ssh

# the legacy SSH transport keeps its historical default of 8 remote slots;
# pass the detected count explicitly when it should match this IDEAPAD
.\tools\dist\build_dist.ps1 -Variant release -Transport ssh -RemoteSlots 12

# explicit capacity overrides
.\tools\dist\build_dist.ps1 -Variant release -LocalSlots 3 -RemoteSlots 10

# leave the tunnel running after the build
.\tools\dist\build_dist.ps1 -Variant release -KeepTunnel
```

`build_app.ps1` uses one trampoline call before the DXVK object build. When the
peer is reachable, the helper starts the tunnel and re-runs the same script with
`WC3_DIST_ACTIVE=1`; the original script body executes unchanged in the child
scope, and the helper stops the tunnel in `finally` after the child exits. No
`try/finally` wraps the build body, so ordinary local builds keep the same code
path. Automatic mode covers all four native variants and uses
`Jobs=local+remote`: the debug corpus objects are `-g0` and run remotely, while
`-g` commands stay on this machine through the localhost-only distcc path and
remain capped by the local slots. `release_optimized_pgo` distributes the
profile-generate training objects. `release_optimized` distributes ThinLTO and
profile-use objects after the merged profile has been copied to IDEAPAD; if
that copy fails, only profile-use commands stay local. Set
`WC3_DIST_JOBS=N` to override the detected Ninja concurrency for the automatic
path. `build_dist.ps1` owns its tunnel explicitly and sets
`WC3_DIST_ACTIVE=1`, so a nested `build_app.ps1` cannot start a second one.

The tunnel can also be managed directly:

```powershell
$t = .\tools\dist\distcc_tunnel.ps1 -Action start
.\tools\dist\distcc_tunnel.ps1 -Action status
.\tools\dist\distcc_tunnel.ps1 -Action stop
```

The start action prints one JSON object with `Port`, `RemoteSlots`,
`LocalSlots`, `Pid`, `PgoProfile` and `PgoProfileSynced`. State lives in
`%LOCALAPPDATA%\wc3-dist\distcc-tunnel.json`; tunnel logs are in
`%LOCALAPPDATA%\wc3-dist\distccd-*.log` and the remote daemon writes
`E:\wc3-dist\distccd-tunnel.log`.

`-Action start` refuses to replace a tunnel whose SSH process is still alive.
This is deliberate: an automatic build or a diagnostic run must not steal the
transport of a build that is already running. The state records the PowerShell
owner in `OwnerPid`. Pass `-Force` only after the owning build has stopped.
`-LocalPort <port>` starts the tunnel on an exact loopback port; use it to
restore a tunnel for a process whose `DISTCC_HOSTS` was already captured (for
example after a dropped SSH connection).

For a C-only target, the same environment can be set by hand:

```powershell
$env:WC3_DIST = '1'
$env:WC3_DIST_TRANSPORT = 'distcc'
$env:WC3_DIST_LOCAL_SLOTS = '4'
$env:CCACHE_PREFIX = 'C:/msys64/ucrt64/bin/python.exe E:/projects/wc3_android_recomp/tools/dist/wc3_dist_cc.py'
$env:DISTCC_HOSTS = '127.0.0.1:<port>/12 localhost/4 --localslots=4 --localslots_cpp=4'
$env:DISTCC_DIR = "$env:LOCALAPPDATA\wc3-dist\distcc-state"
$env:TMPDIR = $env:TEMP
& C:\msys64\ucrt64\bin\ninja.exe -C build\android_app\release\cmake -j 16 <target>
```

`WC3_DIST_DEBUG=1` appends client events to
`%LOCALAPPDATA%\wc3-dist\client.log`. `WC3_DIST_SLOTS` still controls the
number of remote slots for the `ssh` transport.

## Eligibility

Of the 373 object commands produced by the configured release graph, 263 have
no final debug info and are distributable; 110 keep debug info and stay on
this machine. `wc3_dist_cc.py` checks the final `-g`/`-g0` state, not the first
occurrence.

A `-g` command cannot use the remote path because distcc preprocesses locally
and the resulting debug metadata no longer matches a direct compile. It is not
a correctness problem for release objects: all measured debug-off objects and
dependency files are byte-identical to the local reference.

## Measured identity and throughput

The corpus numbers below use the real corpus and the configured release flags.
The separate PGO/LTO probe uses a small synthetic translation unit and is
labelled as such.

* Single 2.9 MB corpus file (`war3_seed_0000.c`, 703,712-byte object):
  local 10.16 s, distcc 7.23 s; object SHA-256
  `2E0675C5161FC3FDE706EA29382EB5B8DB0C51B00D9392C1728B0AAFEE6EB9E3`
  and depfile SHA-256
  `E419AD704FFA22C09EAC6FE292D5789092243A555F19C24664EAF82F4BA8803A`
  matched exactly.
* 16 corpus files around the median size (1.41–1.60 MB): four local
  reference workers took 37.95 s; 16 distcc jobs took 23.88 s. Twelve compiles
  ran remotely and four locally. All 16 objects and all 16 depfiles matched.
* 16 small corpus files (19–360 KB): local reference 10.48 s, distributed
  11.44 s. Distribution overhead dominates at this size; this is the expected
  limit, not an identity failure.
* Debug-on `runtime/platform/platform.c`: object SHA-256
  `C794A457161F6346745F9944BC1B5484E77F3209E116A0AA05993079081C2BC2`
  and the depfile matched the direct compile through the local-only path.

The PGO/LTO transport path was probed separately with a small C translation
unit and the same NDK clang.  These are transport/identity probes, not corpus
throughput numbers:

* `-fprofile-generate=... -fprofile-update=atomic`: 2,504-byte object SHA-256
  `F5A3BB597CE06EB5CB30EC0F661F47E580D981846D2A62CD2E578AAD980E8414`
  matched locally and remotely.
* `-flto=thin`: 2,776-byte object SHA-256
  `42E872F6E0DD57C84D6CFEDA8EBE872E6393CE14B4A31EAD48B5C2F1FB644830`
  matched.
* `-fprofile-use=<merged profile>`: 1,080-byte object SHA-256
  `C3086AB35E12EA316CEF91185E48DE311EEFAC8FCCE2B3581B6CFEB4012C2F5B`
  matched after the profile was copied to the same path on IDEAPAD.
* `-fprofile-use=<merged profile> -flto=thin`: 3,552-byte object SHA-256
  `83C5B14069ED7CE8C0517D2578E116004A9E482849D0B3CAF9D54B9FA57BC733`
  matched.
* The same two cases through `ccache_wrapper.exe` + ccache + `CCACHE_PREFIX`
  also matched: profile-generate 2,528 bytes / `99E3BE71...`, and
  profile-use + ThinLTO 3,572 bytes / `18DA0799...`.

A full debug app Ninja build through this path completed with
`local=4 remote=12 jobs=16` and produced and installed the APK. The first
automatic full-build attempt had reached CMake configure and stopped on a
stale local lift tool (`selection24.exe` older than `selection24.c`) before any
corpus compilation; that failure was not a transport failure. A full
`release_optimized` build with the new PGO/LTO transport has not completed yet;
the probes above establish object identity for the affected command shapes.
The automatic path also covers the DXVK probe compiles when they are stale, but
DXVK object identity under distcc has not yet been measured separately.
`build_dist.ps1` and the explicit `-DryRun` path do not start a project build
by themselves.

## Limits

* Only one distributed build at a time. Tunnel start and stop remove stale
  remote `distccd.exe` processes with `taskkill /F /IM distccd.exe`; a second
  concurrent build would lose its worker.
* The remote compiler and sysroot must match the local NDK version and path.
  The source tree does not need to exist on IDEAPAD because the preprocessed
  translation unit is transferred.
* Network cost is per translation unit. Large files with long compile times
  benefit most; small files can be slower than a local compile.
* Automatic distribution covers all four native variants. Debug-info commands
  still run locally through the local-only path; an explicit
  `build_dist.ps1 -Variant debug` follows the same rule. Optimized variants
  distribute ThinLTO and profile-generate commands; profile-use needs a
  successful profile copy to the same absolute path on IDEAPAD. The custom
  `ssh` transport leaves profile-use local because it has no profile copy step.
* `TMPDIR` must name an existing Windows directory and `DISTCC_DIR` must be
  creatable; the scripts create the state directory. Without `TMPDIR`, distcc
  logs `can't use TMPDIR "/tmp"` and falls back to local compilation.
