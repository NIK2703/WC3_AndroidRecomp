#!/usr/bin/env python3
"""ccache prefix that distributes an NDK clang compile to a remote worker.

Two transports are selectable with WC3_DIST_TRANSPORT:

  distcc  - the default; persistent TCP-over-SSH tunnels feed one or more
            remote distccd workers (IDEAPAD and/or the hotspot phone).  distcc
            owns the remote and local compiler slots.
  ssh     - the original helper protocol in tools/dist/compile_one.py; this
            transport uses IDEAPAD only.

Enable either with WC3_DIST=1 and:

  CCACHE_PREFIX="<python.exe> E:/projects/wc3_android_recomp/tools/dist/wc3_dist_cc.py"

ccache keeps owning the cache and the build graph.  On a cache miss the
distributed path preprocesses locally (so headers, macros, __FILE__ and the
dependency file stay local), ships the preprocessed translation unit to the
chosen peer, and writes the returned object to the path the build expects.
Anything unusual (debug info, modules, response files) is compiled on this
machine, either through distcc's localhost slots or through a counted local
slot.  PGO and LTO commands are eligible only when the automatic helper
explicitly enables them: profile-generate and ThinLTO objects can be produced
remotely; profile-use requires the helper to have copied the merged profile to
the same absolute path on the peer running it (IDEAPAD today).  Any transport
failure falls back to a normal local compile, so a missing helper cannot break
a build.
"""

import atexit
import json
import os
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

IS_WINDOWS = os.name == "nt"

SOURCE_EXTS = {".c", ".cc", ".cpp", ".cxx", ".c++", ".cp"}
CPP_EXTS = {".cc", ".cpp", ".cxx", ".c++", ".cp"}

VALUE_FLAGS = {
    "-o", "-MF", "-MT", "-MJ", "-I", "-isystem", "-iquote", "-idirafter",
    "-include", "-imacros", "-D", "-U", "-F", "-iframework", "-iprefix",
    "-iwithprefix", "-iwithprefixbefore", "-isysroot", "-resource-dir",
    "--sysroot", "-x", "-target",
}
DROP_FLAGS = {
    "-c", "-E", "-MD", "-MMD", "-M", "-MM", "-fdiagnostics-color",
}

_slot = None
_local_slot = None


def env(name, default=None):
    value = os.environ.get(name)
    return value if value not in (None, "") else default


def state_base():
    """Return the per-user state directory on Windows and POSIX."""
    explicit = env("WC3_DIST_STATE_DIR")
    if explicit:
        return Path(explicit)
    if IS_WINDOWS:
        return Path(env("LOCALAPPDATA", tempfile.gettempdir())) / "wc3-dist"
    base = env("XDG_STATE_HOME", str(Path.home() / ".local" / "state"))
    return Path(base) / "wc3-dist"


def log(message):
    if env("WC3_DIST_DEBUG") != "1":
        return
    try:
        base = state_base()
        base.mkdir(parents=True, exist_ok=True)
        with open(base / "client.log", "a", encoding="utf-8") as handle:
            handle.write("%s pid=%d %s\n" %
                         (time.strftime("%Y-%m-%dT%H:%M:%S"), os.getpid(), message))
    except OSError:
        pass


def acquire_local_slot():
    """Block until one of the local compiler slots is free.

    The total job count covers both machines, so on a cache miss without a
    remote slot the wrapper must not start an unbounded number of local
    compiles.  Slots are files shared by all wrapper processes of a build;
    stale entries are ignored after half an hour.
    """
    global _local_slot
    try:
        count = int(env("WC3_DIST_LOCAL_SLOTS", str(os.cpu_count() or 4)))
    except ValueError:
        count = os.cpu_count() or 4
    count = max(1, count)
    try:
        slots = state_base() / "local-slots"
        slots.mkdir(parents=True, exist_ok=True)
        deadline = time.monotonic() + float(env("WC3_DIST_LOCAL_WAIT", "300"))
        announced = False
        while True:
            for index in range(count):
                path = slots / ("slot%d" % index)
                try:
                    handle = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                except FileExistsError:
                    try:
                        if time.time() - path.stat().st_mtime > 300:
                            path.unlink()
                    except OSError:
                        pass
                    continue
                os.write(handle, str(os.getpid()).encode("ascii"))
                os.close(handle)
                _local_slot = path
                return True
            if not announced:
                log("waiting for a local compile slot")
                announced = True
            if time.monotonic() >= deadline:
                log("local slot wait timed out; compiling anyway")
                return False
            time.sleep(0.2)
    except OSError as exc:
        log("local slot error: %s" % exc)
    return False


def release_local_slot():
    global _local_slot
    if _local_slot is not None:
        try:
            _local_slot.unlink()
        except OSError:
            pass
        _local_slot = None


def run_local(compiler, args):
    log("local compile: %s" % compiler)
    acquire_local_slot()
    try:
        return subprocess.call([compiler] + args)
    finally:
        release_local_slot()


def ensure_local_shims(compiler, child_env):
    """Make the remote compiler basename executable on this host.

    distcc hands the remote distccd the compiler name exactly as it was given
    on the command line.  On POSIX the wrapper passes ``clang``/``clang++`` so
    the Windows worker resolves its own NDK through PATH.  distcc also needs
    that name locally for preprocessing and for a local fallback, so a tiny
    shim in the state directory forwards to the real local NDK compiler.
    """
    base = os.path.basename(compiler)
    if base.endswith(".exe"):
        base = base[:-4]
    if base not in ("clang", "clang++"):
        return
    target = compiler
    if not os.path.isabs(target):
        target = shutil.which(target) or target
    target = os.path.abspath(target)
    directory = Path(env("WC3_DIST_SHIM_DIR", str(state_base() / "shims")))
    try:
        directory.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        log("shim directory error: %s" % exc)
        return

    names = [(base, target)]
    sibling = os.path.join(os.path.dirname(target),
                           "clang++" if base == "clang" else "clang")
    if os.path.isfile(sibling):
        names.append((os.path.basename(sibling), sibling))
    for name, path in names:
        shim = directory / name
        content = "#!/bin/sh\nexec %s \"$@\"\n" % shlex.quote(path)
        try:
            if shim.is_file() and shim.read_text(encoding="utf-8") == content:
                os.chmod(shim, 0o755)
                continue
            temporary = shim.with_name("%s.%d.tmp" % (name, os.getpid()))
            temporary.write_text(content, encoding="utf-8")
            os.chmod(temporary, 0o755)
            os.replace(temporary, shim)
        except OSError as exc:
            log("shim write failed for %s: %s" % (shim, exc))
    child_env["PATH"] = str(directory) + os.pathsep + child_env.get("PATH", "")


def run_distcc(compiler, args, local_only=False):
    """Hand a command to distcc.

    distcc preprocesses locally, so headers, macros, ``__FILE__`` and the
    dependency file stay on this machine.  Commands that must not leave this
    machine use a localhost-only host list, which makes distcc take one of the
    local compiler slots; this keeps the local concurrency capped even when
    debug info, LTO or another unsupported flag is present.

    On POSIX the compiler is passed by basename.  The Windows worker finds its
    own NDK through PATH, while the local shims answer distcc's preprocessing
    and local retry.  The local command line keeps its original sysroot: a
    nonexistent sysroot is harmless for preprocessed input, and preserving it
    means a failed remote job can retry locally without a rewritten path.
    """
    if IS_WINDOWS:
        distcc = env("WC3_DISTCC_EXE", r"C:\msys64\usr\bin\distcc.exe")
        if not os.path.isfile(distcc):
            log("distcc not found: %s" % distcc)
            return run_local(compiler, args)
    else:
        distcc = env("WC3_DISTCC_EXE") or shutil.which("distcc") or ""
        if not distcc or not (os.path.isfile(distcc) or shutil.which(distcc)):
            log("distcc not found: %s" % distcc)
            return run_local(compiler, args)
    os.environ.setdefault("TMPDIR", tempfile.gettempdir())
    tmpdir = env("TMPDIR")
    if tmpdir and not os.path.isdir(tmpdir):
        os.environ["TMPDIR"] = tempfile.gettempdir()
    state = env("DISTCC_DIR")
    if state:
        try:
            os.makedirs(state, exist_ok=True)
        except OSError:
            pass
    child_env = os.environ.copy()
    # distcc 3.4 appends "-target <build host>" (aarch64-unknown-linux-android)
    # whenever it sees a clang command without a literal "-target".  That
    # replaces the API level in --target=aarch64-linux-android26, so API-gated
    # libc++ declarations such as pthread_cond_clockwait disappear.  The
    # wrapper already supplies the exact NDK compiler and target; disable all
    # of distcc's cross-compilation rewrites.
    child_env["DISTCC_NO_REWRITE_CROSS"] = "1"
    # ccache exports UNCACHED_ERR_FD so a compiler can flag an uncached
    # result.  distcc 3.4 honours the same variable and writes its trace to
    # that descriptor.  Python closes the descriptor before exec, so distcc's
    # later socket() reuses the number and its own verbose trace corrupts the
    # protocol stream.  The wrapper is the compiler from ccache's point of
    # view and never uses the descriptor, so keep distcc off it.
    child_env.pop("UNCACHED_ERR_FD", None)
    if local_only:
        try:
            slots = int(env("WC3_DIST_LOCAL_SLOTS", str(os.cpu_count() or 4)))
        except ValueError:
            slots = 4
        slots = max(1, slots)
        child_env["DISTCC_HOSTS"] = (
            "localhost/{0} --localslots={0} --localslots_cpp={0}".format(slots))
    # CMake stores the NDK compiler with forward slashes.  On Windows the
    # phone shims are named with the Windows backslash form and found through
    # PATH.  On POSIX the basename lets the Windows distccd resolve its own
    # NDK instead of looking for the Termux path.
    basename_mode = env("WC3_DIST_REMOTE_BASENAME",
                        "0" if IS_WINDOWS else "1") == "1"
    if basename_mode:
        distcc_compiler = os.path.basename(compiler)
        if distcc_compiler.endswith(".exe"):
            distcc_compiler = distcc_compiler[:-4]
        ensure_local_shims(compiler, child_env)
    elif IS_WINDOWS:
        distcc_compiler = compiler.replace("/", "\\")
    else:
        distcc_compiler = compiler
    log("distcc%s: %s" % (" local" if local_only else "", distcc_compiler))
    try:
        return subprocess.call([distcc, distcc_compiler] + args, env=child_env)
    except OSError as exc:
        log("distcc launch failed: %s" % exc)
        return run_local(compiler, args)


def find_output(args):
    for index, arg in enumerate(args):
        if arg == "-o" and index + 1 < len(args):
            return args[index + 1]
        if arg.startswith("-o") and len(arg) > 2 and not arg.startswith("-Obj"):
            return arg[2:]
    return None


def find_input(args):
    skip_next = False
    found = []
    for arg in args:
        if skip_next:
            skip_next = False
            continue
        if arg in VALUE_FLAGS:
            skip_next = True
            continue
        if arg.startswith("-"):
            continue
        if os.path.splitext(arg)[1].lower() in SOURCE_EXTS:
            found.append(arg)
    return found[-1] if found else None


def detect_language(compiler, source, args):
    for index, arg in enumerate(args):
        value = None
        if arg == "-x" and index + 1 < len(args):
            value = args[index + 1].lower()
        elif arg.startswith("-x") and len(arg) > 2:
            value = arg[2:].lower()
        if value in ("c", "cpp-output"):
            return "cpp-output"
        if value in ("c++", "c++-cpp-output"):
            return "c++-cpp-output"
        if value in ("objective-c", "objective-c++", "assembler", "assembler-with-cpp"):
            return None
    if "++" in os.path.basename(compiler).lower():
        return "c++-cpp-output"
    extension = os.path.splitext(source)[1].lower()
    if extension in CPP_EXTS:
        return "c++-cpp-output"
    if extension == ".c":
        return "cpp-output"
    return None


def debug_enabled(args):
    """Return True if the final debug-info state of the command is enabled.

    CMake release commands in this project contain both ``-g`` and a later
    ``-g0``.  The last state wins, and modifiers such as ``-gno-column-info``
    do not turn debug info on by themselves.
    """
    enabled = False
    for arg in args:
        if arg == "-g0":
            enabled = False
        elif arg == "-g" or (arg.startswith("-g") and len(arg) == 3 and arg[2].isdigit()):
            enabled = True
        elif arg.startswith(("-ggdb", "-gdwarf-", "-gcodeview",
                             "-gline-tables-only", "-gmodules",
                             "-gsplit-dwarf", "-gstatement-frontiers")):
            enabled = True
    return enabled


def profile_mode(args):
    """Classify PGO flags without treating an update flag as a mode."""
    use = False
    generate = False
    other = False
    for arg in args:
        if arg.startswith(("-fprofile-instr-use", "-fprofile-use")):
            use = True
        elif arg.startswith(("-fprofile-instr-generate", "-fprofile-generate",
                              "-fcs-profile-generate")):
            generate = True
        elif arg.startswith(("-fprofile", "-fcs-profile", "-fauto-profile")):
            other = True
    if use:
        return "use"
    if generate:
        return "generate"
    if other:
        return "other"
    return None


def eligible(compiler, args):
    if env("WC3_DIST") != "1":
        return False
    base = os.path.basename(compiler).lower()
    if base not in ("clang", "clang.exe", "clang++", "clang++.exe"):
        return False
    norm = compiler.replace("\\", "/").lower()
    ndk_root = env("WC3_DIST_NDK_ROOT")
    is_ndk = "/ndk/" in norm or "/android-ndk" in norm
    if ndk_root:
        prefix = ndk_root.replace("\\", "/").lower().rstrip("/") + "/"
        is_ndk = is_ndk or norm.startswith(prefix)
    if not is_ndk:
        return False
    if "-c" not in args:
        return False
    if find_output(args) is None:
        return False
    if any(arg in ("-E", "-S") for arg in args):
        return False
    if debug_enabled(args):
        return False

    allow_lto = env("WC3_DIST_ALLOW_LTO") == "1"
    allow_pgo = env("WC3_DIST_ALLOW_PGO") == "1"
    profile_ready = env("WC3_DIST_PGO_PROFILE_READY") == "1"

    if any(arg.startswith("-flto") for arg in args) and not allow_lto:
        return False
    pgo = profile_mode(args)
    if pgo:
        if not allow_pgo:
            return False
        # A profile-use compile reads the merged profile during codegen.  The
        # tunnel copies it to the same absolute path on IDEAPAD before the
        # first compile; without that copy the command must stay local.
        if pgo == "use" and not profile_ready:
            return False
        if pgo not in ("use", "generate"):
            return False
    if any(arg in ("--coverage", "-ftest-coverage", "-fprofile-arcs") for arg in args):
        return False
    if any(arg.startswith(("-MJ", "-save-temps", "-include-pch", "-fmodules",
                           "-fmodule-", "-fpass-plugin")) for arg in args):
        return False
    if any(arg.startswith("@") for arg in args):
        return False
    if "-Xclang" in args or "-load" in args:
        return False
    source = find_input(args)
    if not source or not os.path.isfile(source):
        return False
    if detect_language(compiler, source, args) is None:
        return False
    return True


def slot_limit():
    """Return the SSH-transport remote slot cap without a fixed default.

    ``WC3_DIST_SLOTS`` is the precise worker count when the caller knows it
    (build_dist.ps1 sets it from the remote CPU probe).  Otherwise use the
    build's total concurrency, then every local CPU; a direct invocation must
    not silently cap a 12-core worker at an old eight-slot constant.
    """
    for name in ("WC3_DIST_SLOTS", "WC3_DIST_JOBS",
                 "CMAKE_BUILD_PARALLEL_LEVEL"):
        raw = env(name)
        if not raw:
            continue
        try:
            value = int(raw)
        except ValueError:
            continue
        if value > 0:
            return value
    return max(1, os.cpu_count() or 1)


def acquire_slot():
    global _slot
    count = slot_limit()
    if count <= 0:
        return None
    try:
        slots = state_base() / "slots"
        slots.mkdir(parents=True, exist_ok=True)
        for index in range(count):
            path = slots / ("slot%d" % index)
            try:
                handle = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            except FileExistsError:
                try:
                    if time.time() - path.stat().st_mtime > 1800:
                        path.unlink()
                except OSError:
                    pass
                continue
            os.write(handle, str(os.getpid()).encode("ascii"))
            os.close(handle)
            _slot = path
            return path
    except OSError as exc:
        log("slot error: %s" % exc)
    return None


def release_slot():
    global _slot
    if _slot is not None:
        try:
            _slot.unlink()
        except OSError:
            pass
        _slot = None


atexit.register(release_slot)
atexit.register(release_local_slot)


def resolve_host():
    explicit = env("WC3_DIST_HOST")
    if explicit:
        return explicit

    # Resolve the peer name through the local DNS resolver, then map its
    # current IPv4 neighbor entry to the IPv6 link-local address and interface
    # index.  Nothing about the address or interface is fixed in this file, so
    # a new DHCP lease or a changed link-local suffix is picked up on the next
    # call.  The old address cache was removed for the same reason.
    script = (
        "$ErrorActionPreference='SilentlyContinue';"
        "$a=(Resolve-DnsName ideapad -Type A | "
        "Where-Object {$_.IPAddress -match '^\\d+\\.'} | Select-Object -First 1).IPAddress;"
        "if(-not $a){$a=[System.Net.Dns]::GetHostAddresses('ideapad') | "
        "Where-Object {$_.AddressFamily -eq 'InterNetwork'} | Select-Object -First 1;"
        "$a=$a.IPAddressToString};"
        "$n=Get-NetNeighbor -IPAddress $a -AddressFamily IPv4 | Select-Object -First 1;"
        "if(-not $n){Test-Connection -ComputerName $a -Count 1 -Quiet|Out-Null;"
        "$n=Get-NetNeighbor -IPAddress $a -AddressFamily IPv4 | Select-Object -First 1};"
        "if($n){$v=Get-NetNeighbor -InterfaceIndex $n.InterfaceIndex -AddressFamily IPv6 | "
        "Where-Object {$_.IPAddress -like 'fe80::*' -and $_.LinkLayerAddress -eq $n.LinkLayerAddress} | "
        "Select-Object -First 1;"
        "if($v){Write-Output ('{0}%{1}' -f $v.IPAddress,$n.InterfaceIndex)}}"
    )
    host = ""
    try:
        proc = subprocess.run(
            ["powershell", "-NoProfile", "-Command", script],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            timeout=20, text=True)
        lines = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
        if lines:
            host = lines[-1]
    except (OSError, subprocess.SubprocessError):
        pass
    return host


def preprocess(compiler, args, source, output):
    command = []
    index = 0
    while index < len(args):
        arg = args[index]
        if arg == "-c":
            command.append("-E")
            index += 1
            continue
        if arg == "-o" and index + 1 < len(args):
            command += ["-o", output]
            index += 2
            continue
        if arg.startswith("-o") and len(arg) > 2 and not arg.startswith("-Obj"):
            command += ["-o", output]
            index += 1
            continue
        command.append(arg)
        index += 1
    log("preprocess: %s" % compiler)
    return subprocess.run(
        [compiler] + command,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))


def remote_args(args, original_source, language):
    result = []
    index = 0
    while index < len(args):
        arg = args[index]
        if arg == original_source:
            index += 1
            continue
        if arg in ("-o", "-MF", "-MT", "-MJ"):
            index += 2
            continue
        if arg.startswith(("-MF", "-MT", "-MJ")) and len(arg) > 3:
            index += 1
            continue
        if arg.startswith("-o") and len(arg) > 2 and not arg.startswith("-Obj"):
            index += 1
            continue
        if arg in DROP_FLAGS:
            index += 1
            continue
        if arg in VALUE_FLAGS:
            index += 2
            continue
        if arg.startswith(("--sysroot=", "-isystem", "-iquote", "-idirafter",
                           "-include", "-imacros", "-iprefix", "-iwithprefix",
                           "-isysroot", "-resource-dir")):
            index += 1
            continue
        if arg.startswith(("-I", "-D", "-U", "-F", "-iframework", "-x")):
            index += 1
            continue
        result.append(arg)
        index += 1
    result += ["-c", "-x", language]
    return result


def ssh_command(host):
    if IS_WINDOWS:
        ssh = env("WC3_DIST_SSH", r"C:\Program Files\Git\usr\bin\ssh.exe")
        key = env("WC3_DIST_KEY", str(Path.home() / ".ssh" / "id_ideapad"))
    else:
        ssh = env("WC3_DIST_SSH", shutil.which("ssh") or "ssh")
        key = env("WC3_DIST_KEY", str(Path.home() / ".ssh" / "id_wc3_pc"))
    python = env("WC3_DIST_REMOTE_PYTHON", "C:/msys64/ucrt64/bin/python.exe")
    script = env("WC3_DIST_REMOTE_SCRIPT", "E:/wc3-dist/compile_one.py")
    user = env("WC3_DIST_USER", "nikita")
    return [
        ssh, "-T", "-i", key,
        "-o", "BatchMode=yes",
        "-o", "ConnectTimeout=8",
        "-o", "StrictHostKeyChecking=accept-new",
        "-o", "HostKeyAlias=ideapad",
        "-o", "WarnWeakCrypto=no",
        "-o", "LogLevel=ERROR",
        "%s@%s" % (user, host),
        '"%s" "%s"' % (python, script),
    ]


def remote_compile(compiler, args, original_source, source, output, language):
    try:
        with open(source, "rb") as handle:
            payload_source = handle.read()
    except OSError as exc:
        log("source read failed: %s" % exc)
        return None

    header = {
        "compiler": compiler.replace("\\", "/"),
        "args": remote_args(args, original_source, language),
        "lang": language,
        "source_name": os.path.basename(source),
        "timeout": int(env("WC3_DIST_TIMEOUT", "900")),
    }
    raw_header = json.dumps(header, separators=(",", ":")).encode("utf-8")
    compressed = zlib.compress(payload_source, 3)
    payload = (struct.pack(">I", len(raw_header)) + raw_header +
               struct.pack(">Q", len(compressed)) + compressed)

    host = resolve_host()
    command = ssh_command(host)
    if not os.path.isfile(command[0]):
        log("ssh not found: %s" % command[0])
        return None

    started = time.monotonic()
    try:
        proc = subprocess.Popen(
            command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        stdout, stderr = proc.communicate(payload, timeout=header["timeout"] + 120)
    except (OSError, subprocess.SubprocessError) as exc:
        log("ssh failed: %s" % exc)
        return None
    elapsed = time.monotonic() - started

    if proc.returncode != 0 or len(stdout) < 4:
        log("transport rc=%s host=%s stderr=%s" %
            (proc.returncode, host, stderr.decode("utf-8", "replace")[-500:]))
        return None

    header_len = struct.unpack(">I", stdout[:4])[0]
    if header_len <= 0 or 4 + header_len > len(stdout):
        log("bad response header")
        return None
    try:
        result = json.loads(stdout[4:4 + header_len].decode("utf-8"))
    except (ValueError, UnicodeDecodeError) as exc:
        log("bad response json: %s" % exc)
        return None

    if result.get("rc") == 125:
        log("helper error: %s" % result.get("stderr", "")[-500:])
        return None
    if result.get("rc") != 0:
        sys.stderr.write(result.get("stderr", ""))
        sys.stderr.flush()
        log("remote compile rc=%s host=%s %.1fs" %
            (result.get("rc"), host, elapsed))
        return int(result.get("rc", 1))

    body = stdout[4 + header_len:]
    try:
        object_bytes = zlib.decompress(body) if body else b""
    except zlib.error as exc:
        log("object decompress failed: %s" % exc)
        return None
    try:
        directory = os.path.dirname(os.path.abspath(output))
        if directory:
            os.makedirs(directory, exist_ok=True)
        temporary = output + (".dist.%d.tmp" % os.getpid())
        with open(temporary, "wb") as handle:
            handle.write(object_bytes)
        os.replace(temporary, output)
    except OSError as exc:
        log("object write failed: %s" % exc)
        return None

    log("remote compile rc=0 host=%s %.1fs bytes=%d" %
        (host, elapsed, len(object_bytes)))
    return 0


def main():
    argv = sys.argv[1:]
    if argv == ["--print-host"]:
        print(resolve_host())
        return 0
    if not argv:
        return 2
    compiler = argv[0]
    args = argv[1:]

    transport = env("WC3_DIST_TRANSPORT", "ssh" if IS_WINDOWS else "distcc")
    if transport not in ("ssh", "distcc"):
        log("unknown transport %r" % transport)
        return run_local(compiler, args)
    if transport == "distcc":
        if not eligible(compiler, args):
            # Host compiles and commands distcc cannot distribute stay local.
            # Do not route them through distcc's localhost slots: the compiler
            # may itself be a ccache wrapper (build_tools.sh uses
            # ~/bin/clang), and distcc's recursion marker then leaks into the
            # nested ccache invocation and aborts the compile.  run_local()
            # already takes a counted local slot, so the concurrency cap is
            # unchanged.
            return run_local(compiler, args)
        return run_distcc(compiler, args)

    if not eligible(compiler, args):
        return run_local(compiler, args)

    source = find_input(args)
    output = find_output(args)
    language = detect_language(compiler, source, args)
    slot = acquire_slot()
    if slot is None:
        log("no remote slot")
        return run_local(compiler, args)

    temp_input = None
    try:
        suffix = ".ii" if language == "c++-cpp-output" else ".i"
        handle, temp_input = tempfile.mkstemp(prefix="wc3dist-", suffix=suffix)
        os.close(handle)

        processed = preprocess(compiler, args, source, temp_input)
        if processed.returncode != 0:
            sys.stderr.buffer.write(processed.stderr)
            sys.stderr.flush()
            return processed.returncode

        result = remote_compile(compiler, args, source, temp_input, output, language)
        if result is None:
            log("falling back to local compile")
            return run_local(compiler, args)
        return result
    finally:
        release_slot()
        if temp_input:
            try:
                os.unlink(temp_input)
            except OSError:
                pass


if __name__ == "__main__":
    sys.exit(main())
