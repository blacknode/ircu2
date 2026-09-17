"""Shared helpers for trust-username (hidden host display) integration tests.

Every user's host is hidden from registration on: the visible host is the
cipher of the client's address (tests/vhost.py, doc/readme.accounting) and,
with TRUST_USERNAME, the visible username loses its leading ``~``.  The hub
runs an iauth stub that forces the tilde onto every username, so a hub
client's real identity is always ``~<user>@<ip>``.
"""

import asyncio

from irc_client import IRCClient
from vhost import CLIENT_IP, VIS_HOST_CLIENT

# The hub does no DNS, so the address a client arrives from is both its
# real host and what its hidden host is derived from.  See vhost.py for
# why that address is the compose network's gateway and not loopback.
REAL_HOST = CLIENT_IP
VIS_HOST = VIS_HOST_CLIENT


def user_from_prefix(prefix: str | None) -> str | None:
    if not prefix or "!" not in prefix:
        return None
    user_host = prefix.split("!", 1)[1]
    if "@" not in user_host:
        return None
    return user_host.split("@", 1)[0]


async def oper_up(client: IRCClient, name: str = "testoper", password: str = "operpass"):
    await client.send(f"OPER {name} {password}")
    msg = await client.wait_for("381", timeout=5.0)
    return msg


async def whois_userline(observer: IRCClient, nick: str) -> tuple[str, str]:
    """(username, host) as RPL_WHOISUSER shows them to ``observer``."""
    await observer.send(f"WHOIS {nick}")
    msgs = await observer.collect_until("318", timeout=5.0)
    whois = [m for m in msgs if m.command == "311"]
    assert len(whois) == 1, f"Expected one RPL_WHOISUSER, got: {msgs}"
    return whois[0].params[2], whois[0].params[3]


async def whois_actual(oper: IRCClient, nick: str) -> tuple[str, str]:
    """(real username, real host) from RPL_WHOISACTUALLY; opers only."""
    await oper.send(f"WHOIS {nick}")
    msgs = await oper.collect_until("318", timeout=5.0)
    actual = [m for m in msgs if m.command == "338"]
    assert len(actual) == 1, f"Expected one RPL_WHOISACTUALLY, got: {msgs}"
    user, _, host = actual[0].params[2].partition("@")
    return user, host


async def add_gline(oper: IRCClient, mask: str, reason: str = "trust username test",
                   *, operforce: bool = False):
    """Add a global G-line on the hub (target *)."""
    prefix = "!" if operforce else ""
    await oper.send(f"GLINE +{prefix}{mask} * 100000 :{reason}")
    for _ in range(8):
        msg = await oper.recv(timeout=3.0)
        if msg.command == "NOTICE" and any("GLINE" in p for p in msg.params):
            return
        if msg.command.startswith("4") or msg.command.startswith("5"):
            raise AssertionError(f"GLINE rejected: {msg}")
    raise AssertionError(f"GLINE for {mask!r} produced no confirmation NOTICE")


async def remove_gline(oper: IRCClient, mask: str):
    """Deactivate a global G-line and verify it is no longer active.

    Global ``GLINE -mask *`` uses ``TStime()`` as lastmod.  A deactivate in
    the same second as the add is ignored (``gline_modify`` no-ops on equal
    lastmod), which left ``~user@ip`` active and poisoned later tests.
    Wait one second, then deactivate; also apply a local override immediately
    so hub clients are safe even if the global update is delayed.
    """
    try:
        while True:
            await oper.recv(timeout=0.2)
    except asyncio.TimeoutError:
        pass

    # Immediate local override on this server (skips lastmod equality check).
    await oper.send(f"GLINE <{mask}")
    try:
        while True:
            await oper.recv(timeout=0.3)
    except asyncio.TimeoutError:
        pass

    await asyncio.sleep(1.1)

    await oper.send(f"GLINE -{mask} *")
    for _ in range(12):
        try:
            msg = await oper.recv(timeout=3.0)
        except asyncio.TimeoutError:
            break
        joined = " ".join(msg.params)
        if msg.command == "NOTICE" and "GLINE" in joined:
            break
        if msg.command == "512":
            return
        if msg.command.startswith("4") or msg.command.startswith("5"):
            raise AssertionError(f"GLINE remove rejected: {msg}")

    try:
        while True:
            await oper.recv(timeout=0.2)
    except asyncio.TimeoutError:
        pass
    await oper.send(f"GLINE {mask}")
    status = None
    for _ in range(12):
        try:
            msg = await oper.recv(timeout=3.0)
        except asyncio.TimeoutError:
            break
        if msg.command == "280" and len(msg.params) >= 7 and msg.params[1] == mask:
            status = msg.params[6]
            break
        if msg.command in ("281", "512"):
            return
    if status is None:
        raise AssertionError(f"GLINE remove for {mask!r}: no list entry after deactivate")
    if status.strip() == "+":
        raise AssertionError(
            f"GLINE {mask!r} still active after deactivate (status {status!r})"
        )
