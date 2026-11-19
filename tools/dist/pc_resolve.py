#!/usr/bin/env python3
"""Resolve a distcc worker without a fixed IP address.

The workers are Windows machines on the same link as this host.  Their DHCP
leases can change, so the stable identity is the Ethernet MAC address.  The
resolver reads the IPv6/IPv4 neighbour table, prefers an IPv6 link-local
address, and returns it together with the interface zone (``fe80::1%wlan0``)
so ``ssh`` can use it directly.  Android hides the neighbour table from
ordinary apps; when root ``su`` is available the table is read through it.
If the table has no entry yet, an optional TCP scan of the local IPv4 subnet
finds hosts with the SSH port open and keeps only an address whose neighbour
entry carries the configured MAC.  A wrong host is therefore never selected.

The known names have their own MAC defaults: ``ideacentre`` (the desktop) and
``ideapad`` (the laptop).  ``--mac ''`` disables MAC matching for a host that
is found by DNS name or by its last known address.  ``--cache FILE`` remembers
the last resolved address and retries it after neighbour discovery; MAC
matching still applies.  The file is rewritten after every successful resolve,
so a changed lease is picked up without storing an address in the source tree.

Run manually:

    python3 tools/dist/pc_resolve.py --name ideacentre --json
    python3 tools/dist/pc_resolve.py --name ideapad --json
    python3 tools/dist/pc_resolve.py --name ideapad --cache ~/.wc3-dist/ideapad.host

Exit codes: 0 resolved, 3 not found, 2 usage/error.
"""

import argparse
import ipaddress
import json
import os
import re
import socket
import struct
import subprocess
import sys
import threading

DEFAULT_MAC = "d0:37:45:3b:03:b4"
DEFAULT_IDEAPAD_MAC = "5c:61:99:28:ea:27"
DEFAULT_MACS = {"ideacentre": DEFAULT_MAC, "ideapad": DEFAULT_IDEAPAD_MAC}
DEFAULT_NAME = "ideacentre"
IP_TOOL = "/system/bin/ip" if os.path.exists("/system/bin/ip") else "ip"

SIOCGIFADDR = 0x8915
SIOCGIFNETMASK = 0x891b


def env(name, default=None):
    value = os.environ.get(name)
    return value if value not in (None, "") else default


def norm_mac(value):
    if not value:
        return None
    digits = re.sub(r"[^0-9A-Fa-f]", "", value)
    if len(digits) != 12:
        return None
    return ":".join(digits[i:i + 2] for i in range(0, 12, 2)).lower()


def mac_to_eui64(mac):
    """Return the link-local address derived from an Ethernet MAC."""
    parts = [int(part, 16) for part in mac.split(":")]
    parts[0] ^= 0x02
    eui = parts[0:3] + [0xFF, 0xFE] + parts[3:6]
    groups = ["%02x%02x" % (eui[i], eui[i + 1]) for i in range(0, 8, 2)]
    return "fe80::" + ":".join(groups).lstrip(":")


def cache_candidates(path):
    """Return addresses from a last-known-address file, newest first."""
    if not path:
        return []
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            return [line.strip() for line in handle
                    if line.strip() and not line.lstrip().startswith("#")]
    except OSError:
        return []


def remember(path, address):
    """Persist a resolved address for the next start."""
    if not path:
        return
    try:
        parent = os.path.dirname(os.path.abspath(path))
        if parent:
            os.makedirs(parent, exist_ok=True)
        temporary = path + ".tmp"
        with open(temporary, "w", encoding="utf-8") as handle:
            handle.write(address + "\n")
        os.replace(temporary, path)
        try:
            os.chmod(path, 0o600)
        except OSError:
            pass
    except OSError:
        pass


def run(command, timeout=3.0):
    try:
        proc = subprocess.run(
            command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            timeout=timeout, text=True, errors="replace")
        if proc.returncode == 0:
            return proc.stdout
    except (OSError, subprocess.SubprocessError):
        pass
    return ""


_su_state = None


def run_private(command, timeout=6.0):
    """Run a diagnostics command, retrying it through ``su`` on Android.

    The direct attempt comes first so a readable /proc entry never needs root.
    ``su`` is attempted only after an empty result, and a failed attempt is
    remembered for the rest of the process.
    """
    global _su_state
    direct = run(command.split(), timeout=min(timeout, 3.0))
    if direct.strip():
        return direct
    if _su_state is False:
        return ""
    if _su_state is None:
        _su_state = bool(run(["sh", "-c", "command -v su"], timeout=2.0).strip())
        if not _su_state:
            return ""
    quoted = " ".join(_shell_quote(part) for part in command.split())
    via_su = run(["su", "-c", quoted], timeout=timeout)
    if not via_su.strip() and _su_state:
        # `su` exists but refused or is unusable; do not pay for it again.
        _su_state = False
    return via_su


def _shell_quote(text):
    return "'" + text.replace("'", "'\\''") + "'"


def interfaces():
    """List network interfaces without /sys/class/net or if_nameindex.

    Android denies both ``socket.if_nameindex()`` and ``/sys/class/net`` to
    ordinary apps, so the interface list comes from ``ip -o link`` (through
    ``su`` when needed) and falls back to ``ifconfig -a``.
    """
    names = []
    text = run_private(IP_TOOL + " -o link show")
    for line in text.splitlines():
        match = re.match(r"\d+:\s+([^\s:@]+)", line)
        if match and match.group(1) not in names:
            names.append(match.group(1))
    if not names:
        text = run(["ifconfig", "-a"], timeout=2.0)
        for line in text.splitlines():
            match = re.match(r"^([^\s:]+):\s+flags=", line)
            if match and match.group(1) not in names:
                names.append(match.group(1))
    preferred = [n for n in names if n.startswith("wlan")]
    rest = [n for n in names
            if n not in preferred and not n.startswith(("lo", "rmnet", "tun",
                                                         "dummy", "sit"))]
    return preferred + rest


def iface_ipv4(name):
    """Return (address, prefix) for an interface using ioctl, then ifconfig."""
    import fcntl
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        request = struct.pack("256s", name.encode()[:15])
        try:
            raw = fcntl.ioctl(sock.fileno(), SIOCGIFADDR, request)
            address = socket.inet_ntoa(raw[20:24])
            raw = fcntl.ioctl(sock.fileno(), SIOCGIFNETMASK, request)
            netmask = socket.inet_ntoa(raw[20:24])
            prefix = ipaddress.IPv4Network("0.0.0.0/%s" % netmask).prefixlen
            return address, prefix
        except OSError:
            pass
    finally:
        sock.close()

    text = run(["ifconfig", name], timeout=2.0)
    match = re.search(r"inet\s+(\d+\.\d+\.\d+\.\d+)"
                      r"(?:\s+netmask\s+(\d+\.\d+\.\d+\.\d+))?", text)
    if not match:
        return None
    address = match.group(1)
    prefix = 24
    if match.group(2):
        prefix = ipaddress.IPv4Network("0.0.0.0/%s" % match.group(2)).prefixlen
    return address, prefix


def default_route_ipv4():
    for command in (IP_TOOL + " route", IP_TOOL + " -4 route"):
        text = run_private(command)
        for line in text.splitlines():
            parts = line.split()
            if not parts or parts[0] != "default":
                continue
            if "dev" in parts:
                return parts[parts.index("dev") + 1]
    return None


def choose_iface(explicit=None):
    if explicit:
        return explicit
    route = default_route_ipv4()
    if route:
        return route
    best = None
    for name in interfaces():
        info = iface_ipv4(name)
        if not info:
            continue
        address, _ = info
        if address.startswith("127."):
            continue
        if name.startswith("wlan"):
            return name
        if best is None:
            best = name
    return best


def neighbor_table(iface):
    """Return a list of (address, mac) from the IPv6 and IPv4 tables."""
    entries = []
    for command in (IP_TOOL + " -6 neigh show" + ((" dev " + iface) if iface else ""),
                    IP_TOOL + " neigh show" + ((" dev " + iface) if iface else "")):
        text = run_private(command)
        for line in text.splitlines():
            parts = line.split()
            if not parts or "lladdr" not in parts:
                continue
            try:
                mac = norm_mac(parts[parts.index("lladdr") + 1])
            except IndexError:
                continue
            if mac:
                entries.append((parts[0].split("%")[0], mac))

    try:
        with open("/proc/net/arp", "r", encoding="utf-8",
                  errors="replace") as handle:
            for line in handle.readlines()[1:]:
                parts = line.split()
                if len(parts) >= 6 and parts[3] != "00:00:00:00:00:00":
                    mac = norm_mac(parts[3])
                    if mac:
                        entries.append((parts[0], mac))
    except OSError:
        pass

    seen = set()
    unique = []
    for address, mac in entries:
        key = (address, mac)
        if key not in seen:
            seen.add(key)
            unique.append(key)
    return unique


def is_link_local(address):
    return address.lower().startswith("fe80:")


def scope_variants(address, iface):
    if "%" in address or not is_link_local(address):
        return [address]
    variants = []
    if iface:
        variants.append("%s%%%s" % (address, iface))
        try:
            variants.append("%s%%%d" % (address, socket.if_nametoindex(iface)))
        except OSError:
            pass
    variants.append(address)
    return variants


def probe(address, port, timeout, iface):
    for candidate in scope_variants(address, iface):
        try:
            with socket.create_connection((candidate, port), timeout=timeout):
                return candidate
        except OSError:
            continue
    return None


def dns_candidates(name):
    result = []
    if not name:
        return result
    for host in (name, name + ".local", name + ".lan"):
        try:
            infos = socket.getaddrinfo(host, None, type=socket.SOCK_STREAM)
        except OSError:
            continue
        for info in infos:
            address = info[4][0]
            if address not in result:
                result.append(address)
    return result


def scan_subnet(iface, port, timeout, workers, mac):
    """Scan the local /24 for an open port, then attribute it by MAC.

    A plain scan cannot prove which machine answered, so the caller must match
    the resulting address against the neighbour table.  Without a readable
    neighbour table this returns an empty list by design.
    """
    info = iface_ipv4(iface)
    if not info:
        return []
    address, prefix = info
    own = ipaddress.IPv4Address(address)
    network = ipaddress.IPv4Interface("%s/%d" % (address, prefix)).network
    if network.prefixlen < 24:
        network = ipaddress.IPv4Network("%s/24" % address, strict=False)
    hosts = list(network.hosts())
    if len(hosts) > 1022:
        hosts = hosts[:1022]

    found = []
    lock = threading.Lock()

    def check(host):
        if host == own:
            return
        try:
            with socket.create_connection((str(host), port), timeout=timeout):
                with lock:
                    found.append(str(host))
        except OSError:
            pass

    batch = max(8, workers)
    for start in range(0, len(hosts), batch):
        threads = [threading.Thread(target=check, args=(host,))
                   for host in hosts[start:start + batch]]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        # Stop early when the neighbour table already proves the target.
        if any(entry_mac == mac for _, entry_mac in neighbor_table(iface)):
            break
    return found


def resolve(args):
    mac = norm_mac(args.mac) if args.mac else None
    if args.mac and not mac:
        raise SystemExit("pc_resolve: invalid MAC address: %s" % args.mac)
    iface = choose_iface(args.iface)
    if not iface:
        raise SystemExit("pc_resolve: no local network interface with IPv4")

    def found(address, source):
        result = {
            "address": address,
            "family": "ipv6" if ":" in address else "ipv4",
            "iface": iface,
            "mac": mac or "",
            "source": source,
        }
        remember(args.cache, address)
        return result

    def neighbour_addresses():
        table = neighbor_table(iface)
        matches = [address for address, entry_mac in table
                   if mac and entry_mac == mac]
        matches.sort(key=lambda a: (0 if is_link_local(a) else 1,
                                    ":" not in a))
        return matches

    def matches_mac(address):
        if not mac:
            return True
        bare = address.split("%", 1)[0].lower()
        return any(addr.split("%", 1)[0].lower() == bare and entry_mac == mac
                   for addr, entry_mac in neighbor_table(iface))

    explicit = []
    if args.host:
        explicit.append(args.host)
    if args.ipv6:
        explicit.append(args.ipv6)
    if args.ipv4:
        explicit.append(args.ipv4)

    for address in explicit:
        hit = probe(address, args.port, args.timeout, iface)
        if hit:
            return found(hit, "override")

    for address in neighbour_addresses():
        hit = probe(address, args.port, args.timeout, iface)
        if hit:
            return found(hit, "neighbour")

    for address in dns_candidates(args.name):
        hit = probe(address, args.port, args.timeout, iface)
        if not hit:
            continue
        if mac:
            if matches_mac(hit):
                return found(hit, "name+neighbour")
        else:
            return found(hit, "name")

    if mac:
        derived = mac_to_eui64(mac)
        hit = probe(derived, args.port, args.timeout, iface)
        if hit:
            return found(hit, "mac-eui64")

    for address in cache_candidates(args.cache):
        hit = probe(address, args.port, args.timeout, iface)
        if not hit:
            continue
        if mac:
            if matches_mac(hit):
                return found(hit, "cache+neighbour")
        else:
            return found(hit, "cache")

    if mac and args.scan != "never":
        scan_subnet(iface, args.port, min(args.timeout, 0.4), 32, mac)

    for address in neighbour_addresses():
        hit = probe(address, args.port, args.timeout, iface)
        if hit:
            return found(hit, "scan" if mac and args.scan != "never"
                         else "neighbour")

    if mac:
        detail = "no host with MAC %s answered on port %d (%s)" % (
            mac, args.port, iface)
    else:
        detail = "no host named %s or cached address answered on port %d (%s)" % (
            args.name, args.port, iface)
    raise SystemExit(
        "pc_resolve: %s; set WC3_PC_HOST, WC3_IDEAPAD_HOST or --host explicitly"
        % detail)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    # Keep None distinct from an explicit empty --mac: empty disables MAC
    # matching for a name/cache-only worker such as a laptop with an unknown
    # adapter MAC.
    parser.add_argument("--mac", default=None)
    parser.add_argument("--iface", default=env("WC3_PC_IFACE"))
    parser.add_argument("--host", default=env("WC3_PC_HOST"))
    parser.add_argument("--ipv6", default=env("WC3_PC_IPV6"))
    parser.add_argument("--ipv4", default=env("WC3_PC_IPV4"))
    parser.add_argument("--name", default=env("WC3_PC_NAME", DEFAULT_NAME))
    parser.add_argument("--cache", default=env("WC3_PC_CACHE"))
    parser.add_argument("--port", type=int,
                        default=int(env("WC3_PC_SSH_PORT", "22")))
    parser.add_argument("--timeout", type=float, default=2.0)
    parser.add_argument("--scan", choices=("auto", "never", "always"),
                        default=env("WC3_PC_SCAN", "auto"))
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    if args.mac is None:
        args.mac = env("WC3_PC_MAC")
    if args.mac is None:
        args.mac = DEFAULT_MACS.get(args.name.lower(), DEFAULT_MAC)
    if args.name.lower() == "ideapad":
        args.iface = args.iface or env("WC3_IDEAPAD_IFACE")
        args.host = args.host or env("WC3_IDEAPAD_HOST")
        args.ipv6 = args.ipv6 or env("WC3_IDEAPAD_IPV6")
        args.ipv4 = args.ipv4 or env("WC3_IDEAPAD_IPV4")

    result = resolve(args)
    if args.json:
        print(json.dumps(result, sort_keys=True))
    else:
        print(result["address"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
