"""Host/network facts the CLI scripts use to explain themselves (macOS).

Nothing here is part of the wire protocol -- it exists so a failed push can say
*why* it failed instead of hanging silently.
"""
import subprocess


def lan_ip():
    """This Mac's en0 IPv4 (Wi-Fi, usually), or '' if offline / not on en0."""
    try:
        out = subprocess.run(
            ["ipconfig", "getifaddr", "en0"],
            capture_output=True,
            text=True,
            timeout=2,
            check=False,
        )
        return out.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def _is_ipv4(addr):
    parts = addr.split(".")
    return len(parts) == 4 and all(p.isdigit() and int(p) < 256 for p in parts)


def same_subnet(a, b):
    """True if two addresses share a /24. Rough, but right for home LANs.

    Returns True when either side isn't a literal IPv4 address -- we can't tell,
    so we don't cry wolf.
    """
    if not _is_ipv4(a) or not _is_ipv4(b):
        return True
    return a.rsplit(".", 1)[0] == b.rsplit(".", 1)[0]


def tunnel_interfaces():
    """Up tunnel interfaces carrying IPv4 -- i.e. a VPN is probably running.

    A VPN that claims the default route can swallow LAN traffic, which looks
    exactly like a firewall drop from the push side.
    """
    try:
        out = subprocess.run(
            ["ifconfig"], capture_output=True, text=True, timeout=3, check=False
        ).stdout
    except (OSError, subprocess.SubprocessError):
        return []

    found = []
    name = ""
    for line in out.splitlines():
        if line and not line[0].isspace():
            name = line.split(":", 1)[0]
        elif name.startswith(("utun", "ppp", "ipsec")) and line.strip().startswith(
            "inet "
        ):
            found.append(name)
    return sorted(set(found))
