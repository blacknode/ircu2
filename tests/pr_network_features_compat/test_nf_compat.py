"""NETWORK_FEATURES rolling-upgrade compat tests.

Topology (see docker-compose ircd-nf-{a,b,c}):

    A (prod release, u2.10.12.19) — B (tree, NETWORK_FEATURES=FALSE) — C (tree, NETWORK_FEATURES=TRUE)
                                                                              ^
                                                                       services (P10)

+z TLS fingerprint tokens on NICK/umode bursts are a newer extension.  With
NETWORK_FEATURES=FALSE on the middle hop B, those must not reach A.

TOPIC lines from current servers include a topic-who field that older
``ms_topic`` never stored; topic text remains ``parv[parc-1]``, so prod
must accept TOPIC-with-who without desync.

TLS clients on B still get umode +z locally, but B omits the fingerprint
parameter when introducing them toward peers.  C (NF=TRUE) must accept that
+z-without-fingerprint NICK without crashing or protocol-violating.

Accounts are not part of this suite any more: there is no ACCOUNT message
and +r takes no parameter (doc/readme.accounting), which a prod release
cannot parse -- a network that identifies users cannot keep a u2.10.12
server in it.  Users without +r still burst to A as before.
"""

from __future__ import annotations

import asyncio

import pytest

from irc_client import IRCClient
from p10_server import P10Server, strip_msg_tags

pytestmark = pytest.mark.nf_compat

# SHA-256-sized hex token used as a synthetic TLS client fingerprint.
FAKE_TLS_FINGERPRINT = (
    "aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899"
)


@pytest.fixture
async def services(ircd_nf_compat):
    """U:lined P10 services attached to C (NETWORK_FEATURES=TRUE)."""
    c = ircd_nf_compat["c"]
    srv = P10Server(
        name="services.test.net",
        numeric=4,
        password="testpass",
    )
    await srv.connect(c["host"], c["server_port"])
    await srv.handshake()
    yield srv
    await srv.disconnect()


@pytest.fixture
async def spy_on_b(ircd_nf_compat):
    """P10 peer on B to observe what B relays toward other servers (incl. A)."""
    b = ircd_nf_compat["b"]
    spy = P10Server(
        name="spy.test.net",
        numeric=5,
        password="testpass",
        description="NF compat wire spy",
    )
    await spy.connect(b["host"], b["server_port"])
    await spy.handshake()
    yield spy
    await spy.disconnect()


@pytest.fixture
async def spy_on_c(ircd_nf_compat):
    """P10 peer on C to observe what C relays (NF=TRUE hop before B)."""
    c = ircd_nf_compat["c"]
    spy = P10Server(
        name="spyc.test.net",
        numeric=6,
        password="testpass",
        description="NF compat wire spy on C",
    )
    await spy.connect(c["host"], c["server_port"])
    await spy.handshake()
    yield spy
    await spy.disconnect()


async def _make_oper(server: dict, nick: str = "nfoper") -> IRCClient:
    """Register and oper-up with +g so protocol_violation WALLOPS are visible."""
    oper = IRCClient()
    await oper.connect(server["host"], server["port"])
    await oper.register(nick, "oper", "NF Compat Oper")
    await oper.send("OPER testoper operpass")
    await oper.wait_for("381", timeout=5.0)  # RPL_YOUREOPER
    # Ensure +g (debug / desynch) in case HIS_DEBUG_OPER_ONLY flipped it.
    await oper.send(f"MODE {nick} +g")
    await asyncio.sleep(0.2)
    # Drain greeting noise so later WALLOPS checks are clean.
    await _drain(oper, 0.3)
    return oper


async def _drain(client: IRCClient, seconds: float) -> list:
    """Read and discard messages for a short window."""
    collected = []
    deadline = asyncio.get_running_loop().time() + seconds
    while True:
        remaining = deadline - asyncio.get_running_loop().time()
        if remaining <= 0:
            break
        try:
            collected.append(await client.recv(timeout=remaining))
        except (asyncio.TimeoutError, TimeoutError):
            break
    return collected


async def _collect_wallops(client: IRCClient, seconds: float = 2.0) -> list[str]:
    """Collect WALLOPS texts for a window (protocol_violation uses WALLOPS)."""
    msgs = await _drain(client, seconds)
    return [
        m.params[-1]
        for m in msgs
        if m.command.upper() == "WALLOPS" and m.params
    ]


def _topic_lines_for_chan(lines: list[str], chan: str) -> list[str]:
    """P10 TOPIC (T) lines mentioning ``chan``."""
    out = []
    chan_l = chan.lower()
    for line in lines:
        parts = strip_msg_tags(line).split()
        if len(parts) >= 3 and parts[1] == "T" and parts[2].lower() == chan_l:
            out.append(line)
    return out


async def _wait_for_nick_lines(spy: P10Server, nick: str, timeout: float = 8.0) -> list[str]:
    """Collect P10 lines from spy until we see a NICK introducing ``nick``."""
    collected: list[str] = []
    deadline = asyncio.get_running_loop().time() + timeout
    needle = f" N {nick} "
    while asyncio.get_running_loop().time() < deadline:
        remaining = deadline - asyncio.get_running_loop().time()
        try:
            batch = await spy.recv_until("N", timeout=max(0.1, remaining))
        except (asyncio.TimeoutError, TimeoutError):
            break
        collected.extend(batch)
        hits = [line for line in collected if needle in f" {line} "]
        if hits:
            return hits
    return [line for line in collected if needle in f" {line} "]


async def test_topic_with_who_accepted_by_prod(ircd_nf_compat, spy_on_b):
    """TOPIC carrying topic-who must reach prod without protocol_violation.

    Current servers send ``TOPIC chan chanTS topicTS who :text``.  Older
    ``ms_topic`` takes the topic from ``parv[parc-1]`` and ignores the
    who field; prod must keep the topic text and stay linked.
    """
    a = ircd_nf_compat["a"]
    c = ircd_nf_compat["c"]
    chan = "#nf_topic_who"

    setter = IRCClient()
    await setter.connect(c["host"], c["port"])
    await setter.register("twhoset", "testuser", "Topic Setter")

    prod_user = IRCClient()
    await prod_user.connect(a["host"], a["port"])
    await prod_user.register("twhousr", "testuser", "Topic Prod User")

    oper = await _make_oper(a, "twhooper")

    try:
        await setter.send(f"JOIN {chan}")
        await setter.wait_for("JOIN")
        await asyncio.sleep(0.4)

        await spy_on_b.drain_messages(0.3)
        before = len(spy_on_b.received)

        await setter.send(f"TOPIC {chan} :who-compat topic")
        await asyncio.sleep(0.8)
        await spy_on_b.drain_messages(1.0)

        topic_lines = _topic_lines_for_chan(spy_on_b.received[before:], chan)
        assert topic_lines, (
            f"Spy on B never saw TOPIC for {chan}: {spy_on_b.received[before:]!r}"
        )
        matched = [l for l in topic_lines if "who-compat topic" in l]
        assert matched, f"TOPIC text missing on wire: {topic_lines!r}"
        # Format: <src> T <chan> <chanTS> <topicTS> <who> :<topic>
        parts = strip_msg_tags(matched[0]).split()
        assert len(parts) >= 6, f"TOPIC-with-who too short: {matched[0]!r}"
        assert parts[5] == "twhoset", (
            f"Expected who=twhoset on wire, got {parts[5]!r} in {matched[0]!r}"
        )

        await prod_user.send(f"JOIN {chan}")
        msgs = await prod_user.collect_until("366", timeout=8.0)
        topic_msgs = [m for m in msgs if m.command == "332"]
        assert topic_msgs, (
            f"Prod client should receive RPL_TOPIC, got {[m.command for m in msgs]}"
        )
        assert topic_msgs[0].params[-1] == "who-compat topic"

        wallops = await _collect_wallops(oper, seconds=1.5)
        violations = [w for w in wallops if "Protocol Violation" in w]
        assert not violations, (
            "Prod protocol-violated on TOPIC-with-who: " + "; ".join(violations)
        )

        await prod_user.send("PING :after-topic")
        await prod_user.wait_for("PONG", timeout=5.0)
    finally:
        for client in (setter, prod_user, oper):
            try:
                await client.send("QUIT :cleanup")
            except Exception:
                pass
            await client.disconnect()


async def test_plus_z_fingerprint_not_relayed_to_prod(
    ircd_nf_compat, services, spy_on_b
):
    """TLS +z fingerprint on NICK/umode must not be regenerated toward A.

    C (NETWORK_FEATURES=TRUE) accepts and stores the fingerprint.  B
    (NETWORK_FEATURES=FALSE) must omit it from umode_str() when
    re-bursting, so the wire toward A (and other peers of B) never
    carries the fingerprint token.
    """
    a = ircd_nf_compat["a"]
    oper = await _make_oper(a, "nfoper4")

    # modes="+iz <fp>" becomes two IRC params after host: +iz and the fp,
    # matching umode_str()'s "+iwz <fingerprint>" S2S layout.
    nick = "fpuser1"
    await services.introduce_user(
        nick,
        modes=f"+iz {FAKE_TLS_FINGERPRINT}",
        realname="TLS FP User",
    )

    try:
        user_nicks = await _wait_for_nick_lines(spy_on_b, nick, timeout=8.0)
        assert user_nicks, f"Spy on B never saw NICK for {nick!r}"

        for line in user_nicks:
            assert FAKE_TLS_FINGERPRINT not in line, (
                "B relayed +z TLS fingerprint toward peers (incl. prod A): "
                f"{line}"
            )

        wallops = await _collect_wallops(oper, seconds=1.5)
        violations = [w for w in wallops if "Protocol Violation" in w]
        assert not violations, (
            "Unexpected protocol violation on prod after +z fingerprint "
            "introduction: " + "; ".join(violations)
        )
    finally:
        try:
            await oper.send("QUIT :cleanup")
        except Exception:
            pass
        await oper.disconnect()


async def test_tls_plus_z_without_fingerprint_accepted_on_nf_true(
    ircd_nf_compat, services
):
    """TLS client on B (NF=FALSE) reaches C (NF=TRUE) as +z with no fingerprint.

    B omits the fingerprint parameter from S2S NICK while NETWORK_FEATURES is
    off.  C must still SetTLS from bare +z (the ``*(p + 1)`` guard skips
    consuming a missing param) — no crash, no protocol violation, and WHOIS
    on C still reports a secure connection (671).
    """
    b = ircd_nf_compat["b"]
    c = ircd_nf_compat["c"]
    nick = "nftls1"

    oper_c = await _make_oper(c, "nftlsop")

    tls_user = IRCClient()
    await tls_user.connect_tls(b["host"], b["tls_port"])
    await tls_user.register(nick, "testuser", "TLS on NF=FALSE")

    observer = IRCClient()
    await observer.connect(c["host"], c["port"])
    await observer.register("nftlsobs", "testuser", "Observer on C")

    try:
        # C (and services attached to it) must learn the nick from B.
        numnick = await services.wait_for_user(nick, timeout=10.0)
        assert numnick, f"{nick} never appeared on C/services"

        await observer.send(f"WHOIS {nick}")
        whois = await observer.collect_until("318", timeout=5.0)
        secure = [m for m in whois if m.command == "671"]
        assert secure, (
            f"C should keep IsTLS for {nick} introduced without fingerprint; "
            f"WHOIS replies: {[m.command for m in whois]}"
        )

        wallops = await _collect_wallops(oper_c, seconds=1.5)
        violations = [w for w in wallops if "Protocol Violation" in w]
        assert not violations, (
            "C protocol-violated on +z without fingerprint: "
            + "; ".join(violations)
        )

        # C must still be healthy after accepting the NICK.
        await observer.send("PING :after-tls-z")
        pong = await observer.wait_for("PONG", timeout=5.0)
        assert "after-tls-z" in (pong.params[-1] if pong.params else "")
    finally:
        for client in (tls_user, observer, oper_c):
            try:
                await client.send("QUIT :cleanup")
            except Exception:
                pass
            await client.disconnect()


async def test_p10_plus_z_without_fingerprint_no_crash(
    ircd_nf_compat, services
):
    """Direct P10 NICK with +z and no fingerprint param is accepted on C.

    Mimics the wire format B emits under NETWORK_FEATURES=FALSE, injected
    from services (also attached to C) so the receive path is exercised
    without requiring a TLS client.
    """
    c = ircd_nf_compat["c"]
    nick = "nftls2"

    oper_c = await _make_oper(c, "nftlsop2")
    observer = IRCClient()
    await observer.connect(c["host"], c["port"])
    await observer.register("nftlsobs2", "testuser", "Observer on C")

    try:
        # Bare +iz — no fingerprint after the mode string (unlike "+iz <fp>").
        await services.introduce_user(nick, modes="+iz", realname="No FP User")
        await asyncio.sleep(0.4)

        await observer.send(f"WHOIS {nick}")
        whois = await observer.collect_until("318", timeout=5.0)
        assert any(m.command == "311" for m in whois), (
            f"{nick} missing after +z-without-fp introduce: "
            f"{[m.command for m in whois]}"
        )
        secure = [m for m in whois if m.command == "671"]
        assert secure, (
            f"IsTLS not set on C for +z without fingerprint: "
            f"{[m.command for m in whois]}"
        )

        wallops = await _collect_wallops(oper_c, seconds=1.5)
        violations = [w for w in wallops if "Protocol Violation" in w]
        assert not violations, (
            "Unexpected protocol violation on +z-without-fp NICK: "
            + "; ".join(violations)
        )

        await observer.send("PING :after-p10-z")
        await observer.wait_for("PONG", timeout=5.0)
    finally:
        for client in (observer, oper_c):
            try:
                await client.send("QUIT :cleanup")
            except Exception:
                pass
            await client.disconnect()


