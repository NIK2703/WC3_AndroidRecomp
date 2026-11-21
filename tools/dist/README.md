# Distributed C/C++ compilation on IDEAPAD and the hotspot phone

Accelerator for the canonical Android build. Every native build (`release` or
`debug`) automatically uses whichever workers are reachable: IDEAPAD, the
local-network hotspot phone (POCO F5, Termux), or both. Either worker is
optional; with neither one the build is a plain local build. `debug` uses both;
`release` uses IDEAPAD by default and excludes the phone unless opted in. The
internal PGO training pass is `-Variant release -Pgo generate`, not a third
variant. `build_dist.ps1` remains the explicit entry point for transport,
capacity and diagnostic overrides. `ccache` remains the compiler launcher. On a
cache miss `wc3_dist_cc.py` either hands the command to a remote `distccd`
through a persistent TCP-over-SSH tunnel (default), or uses the original SSH
helper protocol in `compile_one.py` (IDEAPAD only). If the phone's `sshd` is
down, `phone_tunnel.ps1` first tries to start it through wireless adb and the
phone's root `su`; the phone stays optional either way.

The same scripts also run in the opposite direction: a Termux phone can use the
ideapad laptop, the ideacentre desktop, or both as its distcc workers. The
POSIX native targets `build_app.sh`, `build_apk.sh`, `build_dxvk.sh` and
`build_adrenotools.sh` start every reachable worker automatically on the phone;
see [Termux client to worker machines](#termux-client-to-worker-machines).

IDEAPAD runs the same NDK at the same absolute path and produces byte-identical
objects. The phone runs the native aarch64 build of the same NDK revision; its
object bytes can differ from the Windows-hosted compiler, but the objects are
ABI- and link-compatible. A function compiled on the phone, linked with the
host NDK and run on the POCO F5 returned 0. The local dependency file, macros
and `__FILE__` expansion still come from the local preprocessor.

## Capacity model

The scripts read the capacity of every machine instead of using a fixed split:

| Machine | Source of slot count | Typical count |
| --- | --- | --- |
| this PC | `[Environment]::ProcessorCount` | 4 |
| IDEAPAD | remote logical CPU count via PowerShell | 12 |
| hotspot phone | boosted top-app `Cpus_allowed_list`, else `nproc` | 6-8, or 3 without root |
| Termux phone client | `nproc` (capped by its own cpuset) | 5-7 |
| desktop PC worker | remote logical CPU count via PowerShell | 4 |
| laptop worker | remote logical CPU count via PowerShell (12 here) | 12 |
| Ninja | local + whichever workers are available | 4-24 |

`build_dist.ps1` writes:

```
DISTCC_HOSTS=127.0.0.1:<ideapad-port>/12 127.0.0.1:<phone-port>/8 localhost/4 --localslots=4 --localslots_cpp=4
CMAKE_BUILD_PARALLEL_LEVEL=24
```

The host list only contains the workers that actually started, in this order:
IDEAPAD, then the phone, then the local slots. With both, the first 12 compiles
go to IDEAPAD, the next up to 8 to the phone, and the remaining four run
locally. With only one worker, its term is the first and only remote term. The
local compiler concurrency is capped at four by the `localhost/4` slots, even
though Ninja may have more jobs in flight. The phone's slot count is whatever
the worker measured after the cpuset boost; without root it is the Termux
`nproc`, usually 3.

Commands that cannot be distributed for identity reasons (debug info,
modules, response files, non-clang compilers, unsupported profile modes) are
sent through distcc with a localhost-only host list. They therefore share the
same four local slots. The custom `ssh` transport uses a counted local-slot
file semaphore for the same cap.

ThinLTO and PGO commands are distributable only when the automatic helper or
`build_dist.ps1` enables them for `release`. The eligibility rules are explicit:
`-flto*` needs `WC3_DIST_ALLOW_LTO=1`; profile-generate needs
`WC3_DIST_ALLOW_PGO=1`; profile-use additionally needs
`WC3_DIST_PGO_PROFILE_READY=1`. A final `release` build copies its merged
profile to the same absolute path on IDEAPAD before the first compile, so the
local preprocessor and the remote compiler both see a real profile. Without
that copy, or without IDEAPAD, profile-use commands stay local instead of
failing. The phone is excluded from `release` by default because it has no
merged PGO profile. Set `WC3_DIST_PHONE_OPTIMIZED=1` to include it; LTO commands
may then use the phone, while profile-use jobs fall back locally because the
profile is not copied there. If IDEAPAD is disabled and the phone is not opted
in, a release build is simply local.

`-LocalSlots`, `-RemoteSlots`, `-PhoneSlots` and `-Jobs` override the detected
values. The default `Jobs` is the local slot count plus every available
worker's slots (IDEAPAD and/or phone); `release` counts IDEAPAD by default and
the phone only when `WC3_DIST_PHONE_OPTIMIZED=1`. Debug corpus
objects end in `-g0` and are distributable; commands whose final state keeps
debug info use the localhost-only distcc host list and stay capped by
`LocalSlots`. `-Jobs` only changes how many commands Ninja starts; it does not
remove the distcc local slot limit. Leaving it below the total slots leaves
remote capacity idle.

A ccache hit does not enter distcc at all; it is cheap and finishes without a
slot. ccache itself still runs one local preprocessor per miss, so a burst of
24 misses briefly adds local preprocessing load beyond the four compile slots.
That is the main reason small translation units are not faster when
distributed.

## Prerequisites

Both workers are optional. These are the requirements for each one; if a
worker is not configured, not running or not reachable, the build simply does
not use it.

IDEAPAD needs:

* the same NDK at `E:\Android\Sdk\ndk\29.0.14206865`;
* the self-built distcc 3.4 binaries in `E:\wc3-dist\bin` (`distccd.exe`,
  `msys-2.0.dll`);
* the helper script `E:\wc3-dist\compile_one.py` for the `ssh` transport;
* an SSH server reachable through the existing `id_ideapad` key and `nikita`
  account.

The hotspot phone (POCO F5, Termux) needs:

* Termux with `openssh` and `distcc` (`pkg install -y openssh distcc`);
* the Android NDK r29 at `~/tools/android-ndk-r29`; its `source.properties`
  must be `Pkg.Revision = 29.0.14206865`;
* `sshd` on 8022, with the `id_marble` public key in
  `~/.ssh/authorized_keys`. The script logs in as the Termux UID (`u0_a360`
  here; override with `WC3_PHONE_USER`);
* wireless adb on port 5555 (or `WC3_PHONE_ADB_PORT`/`WC3_PHONE_ADB_SERIAL`)
  and root `su` granted to the adb shell for auto-start. If port 8022 is closed,
  `phone_tunnel.ps1` runs `su` as the Termux UID, asks a live `runsvdir` to
  start the `sshd` service, or starts `sshd` directly if the supervisor is
  gone. The build waits for 8022 and then continues. If adb or root is
  unavailable, the phone is skipped and IDEAPAD or the local build continues;
  `WC3_PHONE_AUTOSTART=0` disables this attempt;
* root through `su` for the top-app cpuset boost. Without root the worker still
  starts, but is limited to the Termux background cpuset (`nproc`, usually 3);
* battery optimization disabled for Termux, and the phantom-process killer
  disabled on Android 12+ (`settings put global
  settings_enable_monitor_phantom_procs false`). The worker holds
  `termux-wake-lock` for the duration of the tunnel.

The phone's NDK is the native aarch64 build. The worker creates executable
shims whose filenames are the exact Windows compiler paths distcc sends, so
`execvp` finds them through the work directory on `PATH`. `wc3_dist_cc.py`
sends the compiler in Windows backslash form, so CMake's forward-slash compiler
path still matches those shims. The worker verifies the NDK revision on every
start and refuses a mismatch unless `WC3_PHONE_ALLOW_VERSION_MISMATCH=1`.
`WC3_HOST_NDK` (or `ANDROID_NDK_ROOT`/`ANDROID_NDK_HOME`) selects the host NDK
whose compiler path becomes the shim name; the default is
`E:\Android\Sdk\ndk\29.0.14206865`.

This machine needs `C:\msys64\usr\bin\distcc.exe`. That location is deliberate:
ccache fails to launch the client with exit 127 from `.local\bin` and temporary
directories, while the MSYS2 `usr\bin` path works through `CCACHE_PREFIX`.

The IDEAPAD peer is resolved through the local DNS name `ideapad`; the current
IPv4 neighbor entry is then matched to its IPv6 link-local address and
interface index. No address, suffix or interface index is stored in the
scripts, so a new DHCP lease or link-local change is picked up on the next
start. `WC3_DIST_HOST` overrides the resolver.

The phone is resolved on every start, with no stored address. The resolver uses
the IPv4 default route only to find the hotspot interface; the SSH candidates
themselves include IPv4 and IPv6: the interface's DNS IPv4 servers, the default
gateway, the IPv4 neighbor entry for the phone's MAC, the IPv6 neighbor entry,
and the link-local address derived from the MAC (`fe80::...` with the interface
zone index). It accepts the first candidate whose port 8022 is reachable. When
none is reachable, the start action reuses the same candidate list for the adb
auto-start; `-Action resolve` stays a pure reachability check. The adb transport
itself is IPv4-only, while SSH can use an IPv6 candidate.

A new DHCP lease or link-local change is picked up on the next start. A tunnel
that is already open stays bound to the address it was opened to: if the phone
changes address mid-build, that tunnel breaks, its commands fall back locally,
and the next start resolves the new address. `WC3_PHONE_HOST` is an explicit
static override and defeats this behavior; leave it unset for a changing
address. `WC3_PHONE_MAC` changes the default `12-23-73-4E-0F-F4` when the phone's
MAC changes. The phone's SSH host key is stored under the `wc3-phone` alias, so
the changing IP does not change the known-host entry. The IDEAPAD and phone
resolvers are independent; each tunnel starts or fails on its own.

Worker environment overrides: `WC3_DIST_IDEAPAD=0` keeps only the phone and
`WC3_DIST_PHONE=0` keeps only IDEAPAD; setting both to `0` forces a local
build. Phone-specific: `WC3_PHONE_HOST`, `WC3_PHONE_MAC`, `WC3_PHONE_USER`,
`WC3_PHONE_SSH_PORT`, `WC3_PHONE_KEY`, `WC3_PHONE_HOME`, `WC3_PHONE_NDK`,
`WC3_PHONE_BOOST=0`, `WC3_PHONE_ALLOW_VERSION_MISMATCH=1`,
`WC3_DIST_PHONE_OPTIMIZED=1`, and `WC3_DIST_PHONE_SLOTS=N`. Auto-start uses
`WC3_PHONE_AUTOSTART=0` to disable, `WC3_PHONE_ADB` for an explicit `adb.exe`,
`WC3_PHONE_ADB_PORT` (default 5555), `WC3_PHONE_ADB_SERIAL` for an already
connected serial, and `WC3_PHONE_UID` to skip UID detection.

## Usage

```powershell
# Release examples assume build/pgo/wc3.profdata exists.  Run
# tools\android\build_release.ps1 (or the training pass below) first, or add
# -Pgo off for the explicit optimized A/B.

# final release build (requires build/pgo/wc3.profdata from the training pass)
.\tools\android\build_app.ps1 -Variant release
.\tools\android\build_apk.ps1 -Release

# explicit optimized A/B without PGO
.\tools\android\build_apk.ps1 -Release -Pgo off

# debug build: both workers are used when reachable
.\tools\android\build_app.ps1 -Variant debug

# IDEAPAD only
$env:WC3_DIST_PHONE = '0'
.\tools\android\build_app.ps1 -Variant release

# phone only (IDEAPAD disabled); debug uses the phone by default, release
# needs WC3_DIST_PHONE_OPTIMIZED=1
$env:WC3_DIST_IDEAPAD = '0'
.\tools\android\build_app.ps1 -Variant debug
# or, for one explicit run:
.\tools\dist\build_dist.ps1 -Variant debug -NoIdeapad

# neither worker: the same canonical build runs locally
.\tools\dist\build_dist.ps1 -Variant release -NoIdeapad -NoPhone

# include the phone in a release build (LTO commands may use it)
$env:WC3_DIST_PHONE_OPTIMIZED = '1'
.\tools\dist\build_dist.ps1 -Variant release

# force a local build for one invocation
$env:WC3_DIST_AUTO = '0'
.\tools\android\build_app.ps1 -Variant release

# explicit transport entry point (distcc is the default)
.\tools\dist\build_dist.ps1 -Variant release

# final release build: copy the merged profile before compiling
.\tools\dist\build_dist.ps1 -Variant release -Pgo use `
    -PgoProfile build/pgo/wc3.profdata

# internal PGO training pass (not a public variant)
.\tools\dist\build_dist.ps1 -Variant release -Pgo generate

# validate resolution, tunnel and environment without starting a build
.\tools\dist\build_dist.ps1 -Variant release -DryRun

# use the original SSH helper transport instead
.\tools\dist\build_dist.ps1 -Variant release -Transport ssh

# the legacy SSH transport probes IDEAPAD's own logical CPU count;
# -RemoteSlots overrides the detected value
.\tools\dist\build_dist.ps1 -Variant release -Transport ssh -RemoteSlots 12

# explicit capacity overrides
.\tools\dist\build_dist.ps1 -Variant release -LocalSlots 3 -RemoteSlots 10

# leave the tunnel running after the build
.\tools\dist\build_dist.ps1 -Variant release -KeepTunnel
```

`build_app.ps1` uses one trampoline call before the DXVK object build. When at
least one worker is reachable, the helper starts its tunnel and re-runs the
same script with `WC3_DIST_ACTIVE=1`; the original script body executes
unchanged in the child scope, and the helper stops the tunnels in `finally`
after the child exits. No `try/finally` wraps the build body, so ordinary local
builds keep the same code path. Automatic mode covers both variants and the
internal release training pass. Debug and ordinary release builds use
`Jobs=local+every available worker`: the debug corpus objects are `-g0` and run
remotely, while `-g` commands stay on this machine through the localhost-only
distcc path and remain capped by the local slots. A `release` training build
distributes the profile-generate objects to IDEAPAD when it is available. A
final `release` distributes ThinLTO and profile-use objects after the merged
profile has been copied to IDEAPAD; if that copy fails, or IDEAPAD is absent,
only profile-use commands stay local. The phone is left out of `release` by
default; set `WC3_DIST_PHONE_OPTIMIZED=1` to include it for LTO commands. Set
`WC3_DIST_IDEAPAD=0` for phone only, `WC3_DIST_PHONE=0` for IDEAPAD only (set
both for local), and `WC3_DIST_JOBS=N` to override the detected Ninja
concurrency for the automatic path. `build_dist.ps1` owns its tunnels
explicitly and sets `WC3_DIST_ACTIVE=1`, so a nested `build_app.ps1` cannot
start a second set.

The tunnels can also be managed directly:

```powershell
$t = .\tools\dist\distcc_tunnel.ps1 -Action start
$t = .\tools\dist\distcc_tunnel.ps1 -Action start -NoIdeapad
$t = .\tools\dist\distcc_tunnel.ps1 -Action start -NoPhone
.\tools\dist\distcc_tunnel.ps1 -Action status
.\tools\dist\distcc_tunnel.ps1 -Action stop

# the phone independently, without the main orchestrator
$p = .\tools\dist\phone_tunnel.ps1 -Action start
.\tools\dist\phone_tunnel.ps1 -Action resolve
.\tools\dist\phone_tunnel.ps1 -Action status
.\tools\dist\phone_tunnel.ps1 -Action stop
```

The main start action prints one JSON object with `Port`, `RemoteSlots`,
`LocalSlots`, `Pid`, `PgoProfile`, `PgoProfileSynced`, `PhonePort`,
`PhoneSlots`, `PhoneTarget` and a nested `Phone` object. A worker that is
disabled or unavailable has zero slots and an empty port; a phone-only start
has `Pid=0` and `Port=0`. If both workers are unavailable the start still
succeeds with zero slots and no state file, which callers treat as a local
build. State lives in `%LOCALAPPDATA%\wc3-dist\distcc-tunnel.json`; the phone
tunnel keeps its own state in `phone-tunnel.json`. Main tunnel logs are
`%LOCALAPPDATA%\wc3-dist\distccd-*.log`, the phone logs are
`phone-tunnel-*.out.log` and `.err.log`, IDEAPAD's daemon writes
`E:\wc3-dist\distccd-tunnel.log`, and the phone's daemon writes
`~/.wc3-dist/distccd.log`. The main `-Action stop` stops both workers; the
phone script is also safe to call on its own. `-NoIdeapad` and `-NoPhone` skip
the corresponding worker for a single explicit start; both switches give a
zero-slot start that callers treat as local. `-PhoneSlots N` and
`-PhoneLocalPort N` pin the phone capacity and loopback port.

`-Action start` refuses to replace a tunnel whose recorded IDEAPAD SSH process
is still alive. This is deliberate: an automatic build or a diagnostic run
must not steal the transport of a build that is already running. A phone-only
start has no main SSH process (`Pid=0`), so a new start replaces it; the
one-build rule still applies. The state records the PowerShell owner in
`OwnerPid`. Pass `-Force` only after the owning build has stopped. `-LocalPort
<port>` starts the tunnel on an exact loopback port; use it to restore a tunnel
for a process whose `DISTCC_HOSTS` was already captured (for example after a
dropped SSH connection).

For a C-only target, the same environment can be set by hand:

```powershell
$env:WC3_DIST = '1'
$env:WC3_DIST_TRANSPORT = 'distcc'
$env:WC3_DIST_LOCAL_SLOTS = '4'
$env:CCACHE_PREFIX = 'C:/msys64/ucrt64/bin/python.exe E:/projects/wc3_android_recomp/tools/dist/wc3_dist_cc.py'
$env:DISTCC_HOSTS = '127.0.0.1:<ideapad-port>/12 127.0.0.1:<phone-port>/8 localhost/4 --localslots=4 --localslots_cpp=4'
$env:DISTCC_DIR = "$env:LOCALAPPDATA\wc3-dist\distcc-state"
$env:TMPDIR = $env:TEMP
& C:\msys64\ucrt64\bin\ninja.exe -C build\android_app\release\cmake -j 24 <target>
```

The automatic transport (`Invoke-Wc3AutoDistributedBuild`) uses the same
variables but puts `DISTCC_DIR` under `build\distcc\state-online-*` so client
locks live in the project build tree and are cleaned there.

Drop the phone term and use `-j 16` when only IDEAPAD is in use; drop the
IDEAPAD term for phone-only, or drop both remote terms for a local build. The
phone-only host list is `127.0.0.1:<phone-port>/8 localhost/4 ...`.

`WC3_DIST_DEBUG=1` appends client events to
`%LOCALAPPDATA%\wc3-dist\client.log`. `WC3_DIST_SLOTS` sets the precise
worker slot count for the `ssh` transport; without it the wrapper uses the
build's total concurrency, then every local CPU, instead of a fixed eight.

## Eligibility

Of the 373 object commands produced by the configured release graph, 263 have
no final debug info and are distributable; 110 keep debug info and stay on
this machine. `wc3_dist_cc.py` checks the final `-g`/`-g0` state, not the first
occurrence.

A `-g` command cannot use the remote path because distcc preprocesses locally
and the resulting debug metadata no longer matches a direct compile. It is not
a correctness problem for release objects: all measured debug-off objects and
dependency files through IDEAPAD are byte-identical to the local reference.
Phone-produced objects are ABI- and link-compatible, but not byte-identical.

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

The hotspot phone was measured through the same managed tunnel:

* Single 3.37 MB corpus file (`ijl15_closed_0000.c`): local reference 5.19 s;
  the phone with the top-app boost completed in 3.44 s. The phone object is
  2,192,680 bytes (SHA-256 starts `83393EF53709E1EE`); the local reference is
  2,192,624 bytes, so the bytes differ as expected from the native aarch64
  host build. A separate function compiled on the phone linked with the host
  NDK and ran on the POCO F5 with exit status 0.
* 16 concurrent wrapper compiles with the exact two-remote host list
  (IDEAPAD 12 + phone 7/8 + local 4): 16/16 succeeded in 4.83 s. The phone's
  `distccd` log recorded `COMPILE_OK` entries for several `game`/`ijl15`
  objects, and IDEAPAD handled the rest; this is a scheduling smoke test, not
  corpus throughput.
* Dry runs with the final worker selection:
  * both: `local=4 remote=12 phone=7 jobs=23 remote_cpu_share=83%`;
  * IDEAPAD only: `local=4 remote=12 phone=0 jobs=16 remote_cpu_share=75%`;
  * phone only (`WC3_DIST_IDEAPAD=0` or `-NoIdeapad`):
    `local=4 remote=0 phone=7-8 jobs=11-12 remote_cpu_share=64-67%`;
  * neither (`-NoIdeapad -NoPhone`): `local=4 remote=0 phone=0 jobs=4
    remote_cpu_share=0%`, with the canonical local build.
  The phone slot count varies with the active top-app cpuset (6-8 on this
  device). The automatic helper was also exercised with
  `WC3_DIST_IDEAPAD=0`: it produced
  `DISTCC_HOSTS=127.0.0.1:<phone-port>/8 localhost/4 ...` and
  `CMAKE_BUILD_PARALLEL_LEVEL=12`, then restored both variables on disable.
* Phone-only transport probe: a 7,866-byte preprocessed C translation unit was
  compiled with the phone as the only host (`127.0.0.1:<phone-port>/8`, no
  localhost term). distcc logged `exec on 127.0.0.1:<phone-port>/8`, the
  compile exited 0 in 0.345 s, and the 1,120-byte object was ELF64 AArch64
  relocatable. This path does not depend on IDEAPAD.

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

A full debug app Ninja build through the IDEAPAD-only path completed with
`local=4 remote=12 jobs=16` and produced and installed the APK. The first
automatic full-build attempt had reached CMake configure and stopped on a
stale local lift tool (`selection.exe` older than `selection.c`) before any
corpus compilation; that failure was not a transport failure. The two-worker
path has been validated by the dry run, the 16-job mixed compile, and the phone
link/run test above. The independent paths are covered by the four dry runs and
the phone-only transport probe; a full app link and install using
phone-produced objects has not been run yet, and a full phone-only app build
has not been run. A full `release` build with the new PGO/LTO
transport has not completed yet; the probes above establish object identity for
the affected command shapes. The automatic path also covers the DXVK probe
compiles when they are stale, but DXVK object identity under distcc has not yet
been measured separately. `build_dist.ps1` and the explicit `-DryRun` path do
not start a project build by themselves.

## Failure behavior

* IDEAPAD and the phone are independent optional workers. The automatic path
  starts whichever is reachable; if neither is, the same build runs locally.
  A phone-only run uses the phone and the local slots; an IDEAPAD-only run uses
  IDEAPAD and the local slots.
* `build_dist.ps1` with the default distcc transport uses whichever worker is
  available. If neither is available it prints a warning and runs the
  canonical build locally; it does not fail just because IDEAPAD is absent.
  `-NoIdeapad` and `-NoPhone` select a subset, and both select the local path.
* If IDEAPAD disappears while the phone is up, only IDEAPAD's commands fall
  back locally; the phone tunnel remains usable. If the phone disappears, its
  commands fall back locally; IDEAPAD remains usable.
* The phone worker normally connects to an existing Termux `sshd`. If port 8022
  is closed, `phone_tunnel.ps1` tries to start it through wireless adb: it
  switches to the Termux UID with `su`, runs `sv up sshd` when `runsvdir` is
  alive, or starts `sshd` directly when the supervisor is gone, then waits for
  8022. If adb is not connected or authorized, root `su` is unavailable, or
  `sshd` still does not come up, the phone is skipped; IDEAPAD or the local
  build continues. This starts the Termux SSH service, not the Termux UI.
  `WC3_PHONE_AUTOSTART=0` restores the old skip-if-down behavior. Termux:Boot
  covers reboots; the adb path also revives a Termux process killed mid-session.
* A direct cold-start `sshd` runs from the adb/root shell. The worker then may
  not obtain the top-app cpuset and reports the smaller `nproc` slot count
  (usually 3). The supervised `sv up` path keeps the normal app context and the
  6-8 slot boost.
* `release` uses IDEAPAD by default. With IDEAPAD disabled and the phone not
  opted in it runs locally; `WC3_DIST_PHONE_OPTIMIZED=1` enables the phone-only
  LTO path. PGO profile-use remains local on the phone.

## Termux client to worker machines

The POSIX launchers can distribute clang to the ideapad laptop and the
ideacentre desktop instead of only receiving work from them.
`tools/android/build_app.sh` calls `tools/dist/distcc_env.sh` after the
GameActivity AAR checks and before the dependency builds, so the corpus, DXVK
and adrenotools all use the same workers. `build_dxvk.sh` and
`build_adrenotools.sh` also enable them when invoked on their own; when called
from `build_app.sh` they detect the already-active parent transport and reuse
it rather than opening a second tunnel. `build_apk.sh` inherits the same path
through `build_app.sh`. Workers are independent and optional: if one is
missing, unreachable, has the wrong NDK or has no `distccd`, the build still
uses the other worker and the local slots.

One-time setup on the phone:

```sh
pkg install -y openssh distcc ccache
cd /storage/emulated/0/Projects/wc3_android_recomp
tools/dist/pc_tunnel.sh setup-key --worker ideapad
tools/dist/pc_tunnel.sh setup-key --worker pc
```

The laptop defaults to `~/.ssh/id_ideapad` and falls back to
`~/.ssh/id_wc3_pc` when that file is absent; the desktop uses
`~/.ssh/id_wc3_pc`. `setup-key` creates the key if needed and copies the public
key with `ssh-copy-id`; enter the Windows account password when asked. If that
copy fails, a PowerShell fallback writes both `%USERPROFILE%\.ssh\authorized_keys`
and, when the directory is accessible,
`%ProgramData%\ssh\administrators_authorized_keys`; it can ask for the password
again. If both automatic paths fail, add `<key>.pub` to the appropriate file on
the worker, using an elevated PowerShell for
`%ProgramData%\ssh\administrators_authorized_keys`. Every later start is
non-interactive. A pre-existing key can be selected with `WC3_IDEAPAD_KEY` or
`WC3_PC_KEY`.

Each worker needs an OpenSSH server, the same NDK revision as the phone, and a
`distccd.exe`. The probe checks `ANDROID_NDK_ROOT`/`ANDROID_NDK_HOME`, the usual
SDK locations (`%LOCALAPPDATA%\Android\Sdk`, `C:\Android\Sdk`,
`E:\Android\Sdk`, ...), and the usual MSYS2, `%USERPROFILE%\.local\bin` and
`E:\wc3-dist\bin` distccd locations. Override a non-standard layout with
`WC3_IDEAPAD_REMOTE_NDK`/`WC3_PC_REMOTE_NDK` and
`WC3_IDEAPAD_DISTCCD`/`WC3_PC_DISTCCD`; the local NDK is checked against the
worker's `Pkg.Revision` unless `WC3_IDEAPAD_ALLOW_VERSION_MISMATCH=1` or
`WC3_PC_ALLOW_VERSION_MISMATCH=1` is set.

No worker is addressed by a stored IPv4 address. On every start
`pc_resolve.py` reads the IPv6/IPv4 neighbour table, matches the laptop MAC
(`5c:61:99:28:ea:27`) or the desktop Ethernet MAC (`d0:37:45:3b:03:b4`) and
probes SSH. It prefers the IPv6 link-local address with the interface zone,
which is also the form used for the SSH config. A new DHCP lease or link-local
address is picked up on the next start. The last successful address is kept in
`~/.wc3-dist/ideapad.host` or `~/.wc3-dist/pc.host` and retried after MAC
discovery; the cache is only a fallback, not the identity. `WC3_IDEAPAD_HOST` and `WC3_PC_HOST` are temporary diagnostic
overrides that bypass discovery; `WC3_IDEAPAD_MAC`, `WC3_PC_MAC`,
`WC3_IDEAPAD_IFACE`, `WC3_PC_IFACE`, `WC3_IDEAPAD_IPV6`, `WC3_PC_IPV6`,
`WC3_IDEAPAD_IPV4` and `WC3_PC_IPV4` are the remaining per-worker overrides.
None is required.

```sh
# inspect the resolved address without logging in to a worker
tools/dist/pc_tunnel.sh resolve --json
tools/dist/pc_tunnel.sh resolve --worker ideapad --json

# start/stop one worker manually
tools/dist/pc_tunnel.sh start --worker ideapad
tools/dist/pc_tunnel.sh status --worker ideapad --json
tools/dist/pc_tunnel.sh stop --worker ideapad

# build with every reachable worker (the default; WC3_DIST_AUTO=0 forces local)
tools/android/build_app.sh --variant release
tools/android/build_dxvk.sh --variant debug
tools/android/build_adrenotools.sh
```

`wc3_dist_enable` starts the laptop and the desktop independently, then builds
`DISTCC_HOSTS` from one loopback term per ready worker plus the local slots.
The automatic path uses every local CPU slot plus every reachable worker's
probed slots; the total Ninja job count is that sum. Each worker reports its
own logical CPU count: the current laptop reports 12 and the desktop 4.
`WC3_DIST_WORKERS=ideapad` (or `pc,ideapad`) is an explicit list and takes
precedence over the per-worker toggles; when it is unset,
`WC3_DIST_IDEAPAD=0` and `WC3_DIST_PC=0` disable one worker.
`WC3_DIST_LOCAL_SLOTS`, `WC3_IDEAPAD_SLOTS`, `WC3_PC_SLOTS` and
`WC3_DIST_JOBS` override the local split, a worker's slot count, and the total
Ninja job count. Leaving them unset uses all local CPU cores and each worker's
probed count. There is no fixed 12-job default in the DXVK
target on either host. Each worker address is `127.0.0.1:<local-port>` through
its own SSH `-L` forward to a distccd bound to `127.0.0.1` on that worker, so
the daemons are not exposed to the LAN. `wc3_dist_cc.py` sends `clang`/`clang++`
by basename; each daemon finds its own NDK through `PATH`, and small local shims
answer distcc's local preprocessing and local retry. The original local
`--sysroot` is kept: it is not used for preprocessed code generation, and a
failed remote job can retry locally without path rewriting. The app and APK
entries probe the workers by default and have no `--jobs` or `--no-dist`
switch; `WC3_DIST_AUTO=0` forces a local build. On a standalone DXVK or
adrenotools target, `--no-dist` or `WC3_DIST_AUTO=0` does the same. In every
case an inherited transport is removed as well, so a nested auto-disabled
target cannot keep using a parent's `CCACHE_PREFIX`/`DISTCC_HOSTS` by
accident.

State and logs live in `~/.wc3-dist` (`WC3_DIST_STATE_DIR` overrides it), one
set per worker: `ideapad-tunnel.env`, `ideapad-ssh.conf`, `ideapad.host`,
`ideapad-tunnel.err.log`, `ideapad-probe.ps1` and `ideapad-daemon.cmd`, with
the same `pc-*` names for the desktop, plus the local `client.log`. Use
`WC3_DIST_DEBUG=1` for the wrapper log. Only one distributed build may run at a
time. `stop` and a stale-start cleanup run `taskkill /F /IM distccd.exe` on the
affected worker only. Do not start a second build while the first one owns the
workers.

## Limits

* Only one distributed build at a time. Tunnel start and stop remove stale
  remote daemons with `taskkill /F /IM distccd.exe` on IDEAPAD and
  `pkill -f '[d]istccd'` on the phone; a second concurrent build would lose
  whichever workers the first build owns.
* The remote compiler and sysroot must match the local NDK revision. IDEAPAD
  uses the same absolute NDK path. The phone uses the same NDK revision at
  `~/tools/android-ndk-r29` and gets executable shims named after the Windows
  compiler paths, so the source tree does not need to exist on either peer.
* Phone object bytes are not guaranteed identical to the Windows-hosted
  compiler. ABI and link compatibility are verified, but a full app link with
  phone-produced objects has not been run yet.
* The phone's capacity follows Android's cpuset. With root the worker pins
  `distccd` to `top-app` and reports the daemon's effective
  `Cpus_allowed_list` (6-8 CPUs here). Without root it reports `nproc`, usually
  3. The count is re-measured on every start, so it can change between builds.
* The phone is excluded from `release` by default. `WC3_DIST_PHONE_OPTIMIZED=1`
  opts in; the PGO profile is copied only to IDEAPAD, so profile-use jobs on
  the phone fall back locally. With IDEAPAD disabled, a release build is local
  unless the phone is opted in for LTO commands.
* Network cost is per translation unit. Large files with long compile times
  benefit most; small files can be slower than a local compile.
* Automatic distribution covers both variants and the internal release
  training pass, and every available worker. Debug-info commands still run
  locally through the local-only path; an explicit `build_dist.ps1 -Variant
  debug` follows the same rule. Release distributes ThinLTO and
  profile-generate commands; profile-use needs a successful profile copy to
  the same absolute path on IDEAPAD. The custom `ssh` transport leaves
  profile-use local because it has no profile copy step, and it does not use
  the phone; with IDEAPAD disabled it builds locally.
* If the phone disappears mid-build, distcc retries those commands locally;
  the IDEAPAD tunnel is unaffected. If IDEAPAD disappears, its commands retry
  locally and the phone tunnel remains usable. The main state records whichever
  workers started; a phone-only start has `Pid=0`/`Port=0` and is still stopped
  by `-Action stop`.
* `TMPDIR` must name an existing Windows directory and `DISTCC_DIR` must be
  creatable; the scripts create the state directory. Without `TMPDIR`, distcc
  logs `can't use TMPDIR "/tmp"` and falls back to local compilation.
