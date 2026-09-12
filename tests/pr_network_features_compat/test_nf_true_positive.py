"""Positive-path NETWORK_FEATURES=TRUE relay checks on the standard hub.

The nf_compat suite asserts suppression toward prod when NF=FALSE.  These
tests mirror the same extension on a fully upgraded hub (default
NETWORK_FEATURES=TRUE) so over-suppression cannot pass silently:

* TAGMSG (TM) is relayed S2S

Wire observations use ``notulined.test.net`` as a P10 spy alongside
U:lined ``services.test.net`` (both Connect blocks exist on the hub).
"""

from __future__ import annotations

import asyncio
import time

import pytest

from irc_client import IRCClient
from p10_server import P10Server, strip_msg_tags

pytestmark = pytest.mark.multi_server


@pytest.fixture
async def services(ircd_network):
    hub = ircd_network["hub"]
    srv = P10Server(
        name="services.test.net",
        numeric=4,
        password="testpass",
    )
    await srv.connect(hub["host"], hub["server_port"])
    await srv.handshake()
    yield srv
    await srv.disconnect()


@pytest.fixture
async def spy(ircd_network):
    """Non-U:lined peer used only as a wire spy on the hub."""
    hub = ircd_network["hub"]
    srv = P10Server(
        name="notulined.test.net",
        numeric=5,
        password="testpass",
        description="NF=TRUE positive-path wire spy",
    )
    await srv.connect(hub["host"], hub["server_port"])
    await srv.handshake()
    yield srv
    await srv.disconnect()


def _lines_for_token(lines: list[str], token: str) -> list[str]:
    out = []
    for line in lines:
        parts = strip_msg_tags(line).split()
        if len(parts) >= 2 and parts[1] == token:
            out.append(line)
    return out


async def _collect_matching(
    spy: P10Server,
    predicate,
    *,
    seconds: float = 2.5,
) -> list[str]:
    before = len(spy.received)
    deadline = asyncio.get_running_loop().time() + seconds
    matched: list[str] = []
    while asyncio.get_running_loop().time() < deadline:
        await spy.drain_messages(0.3)
        matched = [line for line in spy.received[before:] if predicate(line)]
        if matched:
            return matched
        await asyncio.sleep(0.05)
    return matched


async def test_tagmsg_relayed_s2s_when_network_features_true(ircd_network, spy):
    """With NETWORK_FEATURES=TRUE, TAGMSG must appear on the S2S wire (TM)."""
    hub = ircd_network["hub"]
    channel = "#nftrue_tm"

    # Spy-homed nick in the channel so the hub must forward TM to the spy.
    spy_user = await spy.introduce_user("tmspybot")
    ts = int(time.time())
    await spy._send(f"{spy_user} J {channel} {ts}")
    await asyncio.sleep(0.3)

    user = IRCClient()
    await user.connect(hub["host"], hub["port"])
    await user.negotiate_cap(["message-tags"])
    await user.register("tmnfsnd", "testuser", "TAGMSG Sender")

    try:
        await user.send(f"JOIN {channel}")
        await user.wait_for("JOIN", timeout=5.0)
        await asyncio.sleep(0.3)

        await spy.drain_messages(0.3)
        before = len(spy.received)

        await user.send(f"@+example.com/foo=nftrue TAGMSG {channel}")

        deadline = asyncio.get_running_loop().time() + 3.0
        tm_lines: list[str] = []
        while asyncio.get_running_loop().time() < deadline:
            await spy.drain_messages(0.3)
            tm_lines = _lines_for_token(spy.received[before:], "TM")
            if tm_lines:
                break
            await asyncio.sleep(0.05)

        assert tm_lines, (
            f"TAGMSG was not relayed S2S while NETWORK_FEATURES=TRUE: "
            f"{spy.received[before:]!r}"
        )
        assert any(channel in strip_msg_tags(line) for line in tm_lines), (
            f"TM lines missing channel {channel}: {tm_lines!r}"
        )
    finally:
        try:
            await user.send("QUIT :cleanup")
        except Exception:
            pass
        await user.disconnect()


