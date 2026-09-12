"""Virtual host generation, as the ircd does it (ircd/ircd_vhost.c).

Every user's visible host is TEA over its real address under the network's
``virtual_host_key`` (the Security block), rendered in the P10 base64
alphabet as ``xxxxxx.yyyyyy.v4`` or ``.v6``.  Tests use this to predict
the host a client will be given from the IP it connects from.
"""

from __future__ import annotations

import ipaddress

# The key in every tests/**/ircd*.conf Security block.
TEST_KEY = "AbCdEfGhIjKl"

_ALPHABET = (
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789[]"
)
_MASK32 = 0xFFFFFFFF
_DELTA = 0x9E3779B9


def base64toint(s: str) -> int:
    value = 0
    for ch in s:
        value = (value << 6) + _ALPHABET.index(ch)
    return value & _MASK32


def inttobase64(value: int, count: int) -> str:
    out = []
    for _ in range(count):
        out.append(_ALPHABET[value & 63])
        value >>= 6
    return "".join(reversed(out))


def tea(v: tuple[int, int], k: tuple[int, int], x: tuple[int, int]) -> tuple[int, int]:
    y = (v[0] ^ x[0]) & _MASK32
    z = (v[1] ^ x[1]) & _MASK32
    a, b = k
    total = 0
    for _ in range(32):
        total = (total + _DELTA) & _MASK32
        # k[0] and k[1] are the lower half of the 128-bit TEA key; the upper
        # half (the two constants folded into z's round) is zero.
        y = (y + (((((z << 4) & _MASK32) + a) & _MASK32)
                  ^ ((z + total) & _MASK32)
                  ^ (((z >> 5) + b) & _MASK32))) & _MASK32
        z = (z + ((((y << 4) & _MASK32))
                  ^ ((y + total) & _MASK32)
                  ^ (y >> 5))) & _MASK32
    return y, z


def _derive(key: str) -> tuple[int, int]:
    if len(key) != 12 or any(c not in _ALPHABET for c in key):
        raise ValueError(f"bad virtual host key {key!r}")
    return base64toint(key[:6]), base64toint(key[6:])


def _render(x0: int, x1: int) -> str:
    return f"{inttobase64(x0, 6)}.{inttobase64(x1, 6)}"


def vhost(ip: str, key: str = TEST_KEY) -> str:
    """The hidden host the ircd gives a client connecting from ``ip``."""
    k = _derive(key)
    addr = ipaddress.ip_address(ip)
    if isinstance(addr, ipaddress.IPv6Address) and addr.ipv4_mapped:
        addr = addr.ipv4_mapped
    if isinstance(addr, ipaddress.IPv4Address):
        v1 = int(addr)
        for ts in range(65536):
            v = ((k[0] & 0xFFFF0000) + ts) & _MASK32, v1
            body = _render(*tea(v, k, (0, 0)))
            if "[" not in body and "]" not in body:
                return body + ".v4"
        raise RuntimeError("no bracket-free host")
    packed = int(addr)
    v = (packed >> 96) & _MASK32, (packed >> 64) & _MASK32
    for ts in range(65536):
        body = _render(*tea(v, k, (ts, 0)))
        if "[" not in body and "]" not in body:
            return body + ".v6"
    raise RuntimeError("no bracket-free host")


# What a client connecting from the loopback address is hidden as, under
# the test key; every tests/**/ircd*.conf carries that key.
VIS_HOST_LOOPBACK = None  # filled in below, after vhost() exists


def is_vhost(host: str) -> bool:
    """Does ``host`` have the shape of a generated virtual host?"""
    if len(host) != 16 or host[6] != "." or host[13] != ".":
        return False
    if host[13:] not in (".v4", ".v6"):
        return False
    body = host[:6] + host[7:13]
    return all(c in _ALPHABET and c not in "[]" for c in body)


VIS_HOST_LOOPBACK = vhost("127.0.0.1")
