"""Accounting: umode +r, the hidden host every user carries, and who may
change whose modes.  The model is described in doc/readme.accounting.

* Every user is +x from registration on; the visible host is the cipher
  of the user's address (tests/vhost.py) and a user cannot take +x off.
* +r means "identified to the nick in use"; the account is the nick.  It
  is granted or taken away by a server, by a service bot (+S) or in a
  burst -- never by the user -- and it does not survive a nick change.
* WHOIS shows 307 "is a registered user" for a +r user; 330 is gone.
* A service bot may change the modes of any user but an operator.
"""

import asyncio
import time

import pytest

from irc_client import IRCClient, parse_mode_string
from p10_server import P10Server, strip_msg_tags
from vhost import VIS_HOST_LOOPBACK, is_vhost, vhost

pytestmark = pytest.mark.multi_server

SERVICES = "services.test.net"


@pytest.fixture
async def services(ircd_network):
    """A U:lined P10 server linked to the hub."""
    hub = ircd_network["hub"]
    srv = P10Server(name=SERVICES, numeric=4, password="testpass")
    await srv.connect(hub["host"], hub["server_port"])
    await srv.handshake()
    yield srv
    await srv.disconnect()


@pytest.fixture
async def spy(ircd_network):
    """A plain (not U:lined) P10 server linked to the hub."""
    hub = ircd_network["hub"]
    srv = P10Server(name="notulined.test.net", numeric=5, password="testpass")
    await srv.connect(hub["host"], hub["server_port"])
    await srv.handshake()
    yield srv
    await srv.disconnect()


async def _connect(hub, nick, username="testuser"):
    client = IRCClient()
    await client.connect(hub["host"], hub["port"])
    msgs = await client.register(nick, username, "Accounting Test")
    return client, msgs


async def _quit(*clients):
    for c in clients:
        try:
            await c.send("QUIT :cleanup")
        except Exception:
            pass
        await c.disconnect()


async def _whois(observer, nick):
    await observer.send(f"WHOIS {nick}")
    return await observer.collect_until("318", timeout=5.0)


def _has(msgs, numeric):
    return any(m.command == numeric for m in msgs)


async def _umodes(client):
    """The client's own user modes, as MODE <nick> reports them."""
    await client.send(f"MODE {client.nick}")
    msg = await client.wait_for("221", timeout=5.0)
    return msg.params[-1]


async def _wait_mode(client, letter, sign, timeout=5.0):
    """Wait for a MODE on the client carrying ``sign``+``letter``."""
    loop = asyncio.get_running_loop()
    deadline = loop.time() + timeout
    while True:
        remaining = deadline - loop.time()
        if remaining <= 0:
            raise AssertionError(f"no MODE {sign}{letter} seen")
        msg = await client.wait_for("MODE", timeout=remaining)
        applied = parse_mode_string(msg.params[-1])
        if applied.get(letter) == sign:
            return msg


# ---------------------------------------------------------------------------
# Hidden host
# ---------------------------------------------------------------------------

async def test_every_user_is_plus_x_with_a_derived_host(ircd_network):
    """Registration gives +x and a host derived from the client's address."""
    hub = ircd_network["hub"]
    user, _ = await _connect(hub, "acc01")
    observer, _ = await _connect(hub, "acc01o")
    try:
        # RPL_HOSTHIDDEN follows the MOTD: the host is hidden once the
        # client is registered, before the network is told about it.
        hidden = await user.wait_for("396", timeout=5.0)
        assert hidden.params[1] == VIS_HOST_LOOPBACK, hidden.raw

        assert "x" in await _umodes(user)

        whois = await _whois(observer, "acc01")
        userline = [m for m in whois if m.command == "311"]
        assert userline and userline[0].params[3] == VIS_HOST_LOOPBACK, whois
        assert is_vhost(userline[0].params[3])
    finally:
        await _quit(user, observer)


async def test_user_cannot_remove_plus_x(ircd_network):
    hub = ircd_network["hub"]
    user, _ = await _connect(hub, "acc02")
    try:
        await user.send("MODE acc02 -x")
        await user.assert_no_message("MODE", timeout=1.5)
        assert "x" in await _umodes(user)
    finally:
        await _quit(user)


async def test_remote_user_host_is_derived_from_its_ip(ircd_network, services):
    """A user introduced by another server gets the host the hub derives from
    the IP in the NICK, not the host the NICK named."""
    hub = ircd_network["hub"]
    observer, _ = await _connect(hub, "acc03o")
    try:
        await services.introduce_user("acc03r", host="real.example.net",
                                      ip="203.0.113.7")
        await asyncio.sleep(0.3)
        whois = await _whois(observer, "acc03r")
        userline = [m for m in whois if m.command == "311"]
        assert userline, whois
        assert userline[0].params[3] == vhost("203.0.113.7"), userline[0].raw
    finally:
        await _quit(observer)


async def test_same_user_hides_alike_on_every_server(ircd_network):
    """Hub and leaf carry the same key, so they agree on a user's host."""
    hub, leaf = ircd_network["hub"], ircd_network["leaf1"]
    user, _ = await _connect(hub, "acc04")
    on_leaf, _ = await _connect(leaf, "acc04l")
    on_hub, _ = await _connect(hub, "acc04h")
    try:
        await asyncio.sleep(0.5)
        from_hub = [m for m in await _whois(on_hub, "acc04") if m.command == "311"]
        from_leaf = [m for m in await _whois(on_leaf, "acc04") if m.command == "311"]
        assert from_hub and from_leaf
        assert from_hub[0].params[3] == from_leaf[0].params[3] == VIS_HOST_LOOPBACK
    finally:
        await _quit(user, on_leaf, on_hub)


async def test_service_bot_keeps_its_configured_host(ircd_network, services):
    """A bot the network introduces (+S) is not hidden."""
    hub = ircd_network["hub"]
    observer, _ = await _connect(hub, "acc05o")
    try:
        await services.introduce_user("acc05bot", username="bot",
                                      host="bot.services.test.net", modes="+oikS")
        await asyncio.sleep(0.3)
        whois = await _whois(observer, "acc05bot")
        userline = [m for m in whois if m.command == "311"]
        assert userline and userline[0].params[3] == "bot.services.test.net", whois
    finally:
        await _quit(observer)


# ---------------------------------------------------------------------------
# +r
# ---------------------------------------------------------------------------

async def test_user_cannot_set_plus_r(ircd_network):
    hub = ircd_network["hub"]
    user, _ = await _connect(hub, "acc10")
    observer, _ = await _connect(hub, "acc10o")
    try:
        await user.send("MODE acc10 +r")
        await user.assert_no_message("MODE", timeout=1.5)
        assert "r" not in await _umodes(user)
        assert not _has(await _whois(observer, "acc10"), "307")
    finally:
        await _quit(user, observer)


async def test_server_grants_and_revokes_plus_r(ircd_network, services):
    hub = ircd_network["hub"]
    user, _ = await _connect(hub, "acc11")
    observer, _ = await _connect(hub, "acc11o")
    try:
        await services.wait_for_user("acc11")
        await services.send_register("acc11")
        msg = await _wait_mode(user, "r", "+")
        assert msg.prefix == SERVICES, f"MODE should come from the server: {msg.raw}"
        assert "r" in await _umodes(user)

        whois = await _whois(observer, "acc11")
        reg = [m for m in whois if m.command == "307"]
        assert reg, f"expected 307 after +r: {whois}"
        assert reg[0].params[1] == "acc11", reg[0].raw
        assert not _has(whois, "330"), "330 must not be sent any more"

        await services.send_unregister("acc11")
        await _wait_mode(user, "r", "-")
        assert "r" not in await _umodes(user)
        assert not _has(await _whois(observer, "acc11"), "307")
    finally:
        await _quit(user, observer)


async def test_plus_r_in_nick_burst(ircd_network, services):
    hub = ircd_network["hub"]
    observer, _ = await _connect(hub, "acc12o")
    try:
        await services.introduce_user("acc12r", modes="+ir")
        await asyncio.sleep(0.3)
        assert _has(await _whois(observer, "acc12r"), "307")
    finally:
        await _quit(observer)


async def test_plus_r_relayed_in_nick_burst_without_parameter(ircd_network, services, spy):
    """What the hub tells its peers about a +r user: +r, no account token."""
    hub = ircd_network["hub"]
    user, _ = await _connect(hub, "acc13")
    try:
        await services.wait_for_user("acc13")
        await services.send_register("acc13")
        await asyncio.sleep(0.5)

        # Reconnect the spy so the hub bursts the user afresh.
        await spy.disconnect()
        await spy.connect(hub["host"], hub["server_port"])
        await spy.handshake()
        await spy.wait_for_user("acc13")
        nick_lines = [strip_msg_tags(ln) for ln in spy.received if " N acc13 " in ln]
        assert nick_lines, spy.received[-20:]
        head = nick_lines[-1].split(" :", 1)[0]
        parts = head.split()
        # <src> N nick hop ts user host +modes ip numnick
        assert len(parts) == 10, f"unexpected token after +modes: {nick_lines[-1]!r}"
        assert parts[7].startswith("+"), nick_lines[-1]
        assert "r" in parts[7] and "x" in parts[7], nick_lines[-1]
    finally:
        await _quit(user)


async def test_nick_change_drops_plus_r(ircd_network, services):
    hub = ircd_network["hub"]
    user, _ = await _connect(hub, "acc14")
    observer, _ = await _connect(hub, "acc14o")
    try:
        await services.wait_for_user("acc14")
        await services.send_register("acc14")
        await _wait_mode(user, "r", "+")

        await user.send("NICK acc14b")
        await user.wait_for("NICK", timeout=5.0)
        user.nick = "acc14b"
        await _wait_mode(user, "r", "-")
        assert "r" not in await _umodes(user)
        assert not _has(await _whois(observer, "acc14b"), "307")
    finally:
        await _quit(user, observer)


async def test_case_only_nick_change_keeps_plus_r(ircd_network, services):
    hub = ircd_network["hub"]
    user, _ = await _connect(hub, "acc15abc")
    observer, _ = await _connect(hub, "acc15o")
    try:
        await services.wait_for_user("acc15abc")
        await services.send_register("acc15abc")
        await _wait_mode(user, "r", "+")

        await user.send("NICK Acc15ABC")
        await user.wait_for("NICK", timeout=5.0)
        user.nick = "Acc15ABC"
        await user.assert_no_message("MODE", timeout=1.5)
        assert "r" in await _umodes(user)
        assert _has(await _whois(observer, "Acc15ABC"), "307")
    finally:
        await _quit(user, observer)


async def test_remote_nick_change_drops_plus_r_on_hub(ircd_network, services):
    """The rule is applied by every server to the NICK it sees: a remote
    user's nick change drops +r on the hub too, with nothing on the wire."""
    hub = ircd_network["hub"]
    observer, _ = await _connect(hub, "acc16o")
    try:
        numnick = await services.introduce_user("acc16r", modes="+ir")
        await asyncio.sleep(0.3)
        assert _has(await _whois(observer, "acc16r"), "307")
        await services._send(f"{numnick} N acc16s {int(time.time())}")
        await asyncio.sleep(0.3)
        assert not _has(await _whois(observer, "acc16s"), "307")
    finally:
        await _quit(observer)


# ---------------------------------------------------------------------------
# Who may change whose modes
# ---------------------------------------------------------------------------

async def test_service_bot_sets_modes_on_another_user(ircd_network, services):
    hub = ircd_network["hub"]
    user, _ = await _connect(hub, "acc20")
    try:
        bot = await services.introduce_user("acc20bot", modes="+oikS")
        await services.wait_for_user("acc20")
        await services.send_user_mode("acc20", "+r", from_numnick=bot)
        msg = await _wait_mode(user, "r", "+")
        assert msg.prefix and msg.prefix.startswith("acc20bot!"), msg.raw

        await services.send_user_mode("acc20", "-r+i", from_numnick=bot)
        msg = await user.wait_for("MODE", timeout=5.0)
        applied = parse_mode_string(msg.params[-1])
        assert applied.get("r") == "-", msg.raw
        modes = await _umodes(user)
        assert "r" not in modes and "i" in modes
    finally:
        await _quit(user)


async def test_service_bot_cannot_touch_an_operator(ircd_network, services):
    hub = ircd_network["hub"]
    oper, _ = await _connect(hub, "acc21op")
    await oper.send("OPER testoper operpass")
    await oper.wait_for("381", timeout=5.0)
    try:
        bot = await services.introduce_user("acc21bot", modes="+oikS")
        await services.wait_for_user("acc21op")
        await services.send_user_mode("acc21op", "+r", from_numnick=bot)
        await oper.assert_no_message("MODE", timeout=1.5)
        assert "r" not in await _umodes(oper)
    finally:
        await _quit(oper)


async def test_service_bot_cannot_grant_server_only_modes(ircd_network, services):
    """+o, +k, +S, +B and -x are dropped; the rest of the string applies."""
    hub = ircd_network["hub"]
    user, _ = await _connect(hub, "acc22")
    try:
        bot = await services.introduce_user("acc22bot", modes="+oikS")
        await services.wait_for_user("acc22")
        await services.send_user_mode("acc22", "+okSBr-x", from_numnick=bot)
        msg = await _wait_mode(user, "r", "+")
        modes = await _umodes(user)
        assert "r" in modes and "x" in modes, modes
        for letter in "okSB":
            assert letter not in modes, modes
    finally:
        await _quit(user)


async def test_plain_remote_user_cannot_set_modes_on_others(ircd_network, services):
    hub = ircd_network["hub"]
    user, _ = await _connect(hub, "acc23")
    try:
        other = await services.introduce_user("acc23x", modes="+i")
        await services.wait_for_user("acc23")
        await services.send_user_mode("acc23", "+r", from_numnick=other)
        await user.assert_no_message("MODE", timeout=1.5)
        assert "r" not in await _umodes(user)
    finally:
        await _quit(user)


async def test_local_user_cannot_set_modes_on_others(ircd_network):
    hub = ircd_network["hub"]
    a, _ = await _connect(hub, "acc24a")
    b, _ = await _connect(hub, "acc24b")
    try:
        await a.send("MODE acc24b +i")
        msg = await a.wait_for("502", timeout=5.0)
        assert msg.command == "502"
        await b.assert_no_message("MODE", timeout=1.0)
    finally:
        await _quit(a, b)


# ---------------------------------------------------------------------------
# What still keys on +r
# ---------------------------------------------------------------------------

async def test_regonly_channel_admits_plus_r_user(ircd_network, services):
    hub = ircd_network["hub"]
    owner, _ = await _connect(hub, "acc30own")
    user, _ = await _connect(hub, "acc30")
    try:
        await owner.send("JOIN #acc30")
        await owner.wait_for("366")
        await owner.send("MODE #acc30 +r")
        await owner.wait_for("MODE")

        await user.send("JOIN #acc30")
        assert (await user.wait_for("477", timeout=5.0)).command == "477"

        await services.wait_for_user("acc30")
        await services.send_register("acc30")
        await _wait_mode(user, "r", "+")
        await user.send("JOIN #acc30")
        assert (await user.wait_for("366", timeout=5.0)).command == "366"
    finally:
        await _quit(owner, user)


async def test_whox_has_no_account_field(ircd_network, services):
    """WHOX %a is gone: a request for it is ignored, no field appended."""
    hub = ircd_network["hub"]
    user, _ = await _connect(hub, "acc31")
    try:
        await services.wait_for_user("acc31")
        await services.send_register("acc31")
        await _wait_mode(user, "r", "+")
        await user.send("WHO acc31 %na")
        msgs = await user.collect_until("315", timeout=5.0)
        rows = [m for m in msgs if m.command == "354"]
        assert rows, msgs
        # <me> <nick> only: nothing was appended for 'a'.
        assert rows[0].params[1:] == ["acc31"], rows[0].raw
    finally:
        await _quit(user)
