"""Accounting: the account a user is logged in to, and the hidden host
every user carries.  The model is described in doc/readme.accounting.

* Every user is +x from registration on; the visible host is the cipher
  of the user's address (tests/vhost.py) and a user cannot take +x off.
  That half is this server's own and does not come from the services.
* An account is what the network's services say it is.  They send
  ``ACCOUNT`` (token ``AC``) from a U:lined server, or ``+r <account>``
  in a NICK burst; the ircd records it, sets +r and passes it on.  There
  is no ``-r``: this server cannot take away what it did not give.
* WHOIS shows 330 "is logged in as" with the account name.
* A service bot (+S) may change another user's modes, but not an
  operator's, and not the modes only a server may set.
"""

import asyncio
import time

import pytest

from irc_client import IRCClient, parse_mode_string
from p10_server import P10Server, strip_msg_tags
from vhost import VIS_HOST_CLIENT, is_vhost, vhost

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
    # register() stops at the end of the MOTD, but registration is not
    # over: the hidden host (396) and the `MODE <nick> :+x` that goes with
    # it follow.  Several tests here assert that a mode change produced
    # *no* MODE, so that one has to be out of the stream first -- it is
    # returned rather than dropped, since what it says is the subject of
    # the first test in this file.
    msgs += await client.drain()
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


def _account_in(msgs):
    """The account name RPL_WHOISACCOUNT reported, or None."""
    for m in msgs:
        if m.command == "330":
            return m.params[2]
    return None


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


async def _login(services, client, account, acc_id=None, acc_flags=None):
    """Log ``client`` in the way the services do, and wait for +r."""
    await services.send_register(client.nick, account=account, acc_id=acc_id,
                                 acc_flags=acc_flags)
    loop = asyncio.get_running_loop()
    deadline = loop.time() + 5.0
    while True:
        if "r" in await _umodes(client):
            return
        if loop.time() >= deadline:
            raise AssertionError(f"{client.nick} never became +r")
        await asyncio.sleep(0.2)


# ---------------------------------------------------------------------------
# Hidden host
# ---------------------------------------------------------------------------

async def test_every_user_is_plus_x_with_a_derived_host(ircd_network):
    """Registration gives +x and a host derived from the client's address."""
    hub = ircd_network["hub"]
    user, msgs = await _connect(hub, "acc01")
    observer, _ = await _connect(hub, "acc01o")
    try:
        # RPL_HOSTHIDDEN follows the MOTD: the host is hidden once the
        # client is registered, before the network is told about it.
        hidden = [m for m in msgs if m.command == "396"]
        assert hidden, f"no RPL_HOSTHIDDEN in registration: {msgs}"
        assert hidden[0].params[1] == VIS_HOST_CLIENT, hidden[0].raw

        assert "x" in await _umodes(user)

        whois = await _whois(observer, "acc01")
        userline = [m for m in whois if m.command == "311"]
        assert userline and userline[0].params[3] == VIS_HOST_CLIENT, whois
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
        assert from_hub[0].params[3] == from_leaf[0].params[3] == VIS_HOST_CLIENT
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
# The account
# ---------------------------------------------------------------------------

async def test_user_cannot_set_plus_r(ircd_network):
    hub = ircd_network["hub"]
    user, _ = await _connect(hub, "acc10")
    observer, _ = await _connect(hub, "acc10o")
    try:
        await user.send("MODE acc10 +r mine")
        await user.assert_no_message("MODE", timeout=1.5)
        assert "r" not in await _umodes(user)
        assert _account_in(await _whois(observer, "acc10")) is None
    finally:
        await _quit(user, observer)


async def test_account_grants_plus_r_and_whois_reports_it(ircd_network, services):
    """ACCOUNT from the services is what logs a user in."""
    hub = ircd_network["hub"]
    user, _ = await _connect(hub, "acc11")
    observer, _ = await _connect(hub, "acc11o")
    try:
        await _login(services, user, "acc11acct")
        whois = await _whois(observer, "acc11")
        assert _account_in(whois) == "acc11acct", whois
        assert not _has(whois, "307"), "307 is not how an account is reported"
    finally:
        await _quit(user, observer)


async def test_an_account_cannot_be_taken_away(ircd_network, services):
    """There is no -r.  A mode string that asks for one changes nothing."""
    hub = ircd_network["hub"]
    user, _ = await _connect(hub, "acc12")
    observer, _ = await _connect(hub, "acc12o")
    try:
        await _login(services, user, "acc12acct")
        await services.send_user_mode("acc12", "-r")
        await asyncio.sleep(0.5)
        assert "r" in await _umodes(user)
        assert _account_in(await _whois(observer, "acc12")) == "acc12acct"
    finally:
        await _quit(user, observer)


async def test_account_from_a_server_that_is_not_ulined_is_ignored(
        ircd_network, services, spy):
    """Only a U:lined server may say who somebody is."""
    hub = ircd_network["hub"]
    user, _ = await _connect(hub, "acc13")
    observer, _ = await _connect(hub, "acc13o")
    try:
        numnick = await spy.wait_for_user("acc13")
        await spy.send_account(numnick, "stolen")
        await asyncio.sleep(0.7)
        assert "r" not in await _umodes(user)
        assert _account_in(await _whois(observer, "acc13")) is None

        # The same line from the U:lined server is accepted, which is what
        # makes the refusal above about who sent it and not about the line.
        await _login(services, user, "acc13acct")
        assert _account_in(await _whois(observer, "acc13")) == "acc13acct"
    finally:
        await _quit(user, observer)


async def test_account_in_a_nick_burst(ircd_network, services):
    """+r takes the account as its parameter in the NICK that introduces a
    user, which is how an account reaches a server that was not there."""
    hub = ircd_network["hub"]
    observer, _ = await _connect(hub, "acc14o")
    try:
        await services.introduce_user("acc14r", modes="+ir", account="acc14acct")
        await asyncio.sleep(0.3)
        assert _account_in(await _whois(observer, "acc14r")) == "acc14acct"
    finally:
        await _quit(observer)


async def test_account_relayed_in_the_nick_burst(ircd_network, services, spy):
    """What the hub tells a peer about a logged-in user: +r and the name."""
    hub = ircd_network["hub"]
    user, _ = await _connect(hub, "acc15")
    try:
        await _login(services, user, "acc15acct")

        # Reconnect the spy so the hub bursts the user afresh.
        await spy.disconnect()
        await spy.connect(hub["host"], hub["server_port"])
        await spy.handshake()
        await spy.wait_for_user("acc15")
        nick_lines = [strip_msg_tags(ln) for ln in spy.received if " N acc15 " in ln]
        assert nick_lines, spy.received[-20:]
        head = nick_lines[-1].split(" :", 1)[0]
        parts = head.split()
        # <src> N nick hop ts user host +modes <account> ip numnick
        assert len(parts) == 11, f"unexpected NICK shape: {nick_lines[-1]!r}"
        assert parts[7].startswith("+"), nick_lines[-1]
        assert "r" in parts[7] and "x" in parts[7], nick_lines[-1]
        assert parts[8] == "acc15acct", nick_lines[-1]
    finally:
        await _quit(user)


async def test_account_id_and_flags_ride_with_the_name(ircd_network, services, spy):
    """``<account>:<id>:<flags>`` is one parameter; all three cross a link."""
    hub = ircd_network["hub"]
    user, _ = await _connect(hub, "acc16")
    try:
        await _login(services, user, "acc16acct", acc_id=17, acc_flags=3)

        await spy.disconnect()
        await spy.connect(hub["host"], hub["server_port"])
        await spy.handshake()
        await spy.wait_for_user("acc16")
        nick_lines = [strip_msg_tags(ln) for ln in spy.received if " N acc16 " in ln]
        assert nick_lines, spy.received[-20:]
        parts = nick_lines[-1].split(" :", 1)[0].split()
        assert parts[8] == "acc16acct:17:3", nick_lines[-1]
    finally:
        await _quit(user)


async def test_account_is_relayed_to_the_other_servers(ircd_network, services, spy):
    """A login on a running network travels as ACCOUNT, not as a burst."""
    hub = ircd_network["hub"]
    user, _ = await _connect(hub, "acc17")
    try:
        numnick = await spy.wait_for_user("acc17")
        await spy.drain_messages()
        await _login(services, user, "acc17acct")
        await asyncio.sleep(0.5)
        relayed = [strip_msg_tags(ln) for ln in spy.received
                   if f" AC {numnick} " in f"{strip_msg_tags(ln)} "]
        assert relayed, spy.received[-20:]
        assert relayed[-1].split()[3] == "acc17acct", relayed[-1]
    finally:
        await _quit(user)


async def test_the_account_survives_a_nick_change(ircd_network, services):
    """An account is not the nick: changing one does not give up the other."""
    hub = ircd_network["hub"]
    user, _ = await _connect(hub, "acc18")
    observer, _ = await _connect(hub, "acc18o")
    try:
        await _login(services, user, "acc18acct")
        await user.send("NICK acc18b")
        await user.wait_for("NICK", timeout=5.0)
        user.nick = "acc18b"
        await asyncio.sleep(0.4)
        assert "r" in await _umodes(user)
        assert _account_in(await _whois(observer, "acc18b")) == "acc18acct"
    finally:
        await _quit(user, observer)


async def test_a_remote_nick_change_keeps_the_account_on_the_hub(
        ircd_network, services):
    """The same rule applied to a user this server only heard about."""
    hub = ircd_network["hub"]
    observer, _ = await _connect(hub, "acc19o")
    try:
        numnick = await services.introduce_user("acc19r", modes="+ir",
                                                account="acc19acct")
        await asyncio.sleep(0.3)
        assert _account_in(await _whois(observer, "acc19r")) == "acc19acct"
        await services._send(f"{numnick} N acc19s {int(time.time())}")
        await asyncio.sleep(0.3)
        assert _account_in(await _whois(observer, "acc19s")) == "acc19acct"
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
        await services.send_user_mode("acc20", "+d", from_numnick=bot)
        msg = await _wait_mode(user, "d", "+")
        assert msg.prefix and msg.prefix.startswith("acc20bot!"), msg.raw

        await services.send_user_mode("acc20", "-d+i", from_numnick=bot)
        msg = await user.wait_for("MODE", timeout=5.0)
        applied = parse_mode_string(msg.params[-1])
        assert applied.get("d") == "-", msg.raw
        modes = await _umodes(user)
        assert "d" not in modes and "i" in modes
    finally:
        await _quit(user)


async def test_service_bot_cannot_touch_an_operator(ircd_network, services):
    hub = ircd_network["hub"]
    oper, _ = await _connect(hub, "acc21op")
    await oper.send("OPER testoper operpass")
    await oper.wait_for("381", timeout=5.0)
    # OPER answers 381 and then sets the oper's own modes; this test
    # asserts that a *later* change produces no MODE at all, so the one
    # opering up produced has to be out of the way first.
    await oper.drain()
    try:
        bot = await services.introduce_user("acc21bot", modes="+oikS")
        await services.wait_for_user("acc21op")
        await services.send_user_mode("acc21op", "+d", from_numnick=bot)
        await oper.assert_no_message("MODE", timeout=1.5)
        assert "d" not in await _umodes(oper)
    finally:
        await _quit(oper)


async def test_service_bot_cannot_grant_server_only_modes(ircd_network, services):
    """+o, +k, +S, +B and -x are dropped; the rest of the string applies."""
    hub = ircd_network["hub"]
    user, _ = await _connect(hub, "acc22")
    try:
        bot = await services.introduce_user("acc22bot", modes="+oikS")
        await services.wait_for_user("acc22")
        await services.send_user_mode("acc22", "+okSBd-x", from_numnick=bot)
        await _wait_mode(user, "d", "+")
        modes = await _umodes(user)
        assert "d" in modes and "x" in modes, modes
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
        await services.send_user_mode("acc23", "+d", from_numnick=other)
        await user.assert_no_message("MODE", timeout=1.5)
        assert "d" not in await _umodes(user)
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
# What keys on the account
# ---------------------------------------------------------------------------

async def test_regonly_channel_admits_an_identified_user(ircd_network, services):
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

        await _login(services, user, "acc30acct")
        await user.send("JOIN #acc30")
        assert (await user.wait_for("366", timeout=5.0)).command == "366"
    finally:
        await _quit(owner, user)


async def test_whox_reports_the_account(ircd_network, services):
    """WHOX %a is the account name, and "0" for a user who has none."""
    hub = ircd_network["hub"]
    user, _ = await _connect(hub, "acc31")
    other, _ = await _connect(hub, "acc31o")
    try:
        await _login(services, user, "acc31acct")
        await user.send("WHO acc31 %na")
        msgs = await user.collect_until("315", timeout=5.0)
        rows = [m for m in msgs if m.command == "354"]
        assert rows, msgs
        assert rows[0].params[1:] == ["acc31", "acc31acct"], rows[0].raw

        await user.send("WHO acc31o %na")
        msgs = await user.collect_until("315", timeout=5.0)
        rows = [m for m in msgs if m.command == "354"]
        assert rows, msgs
        assert rows[0].params[1:] == ["acc31o", "0"], rows[0].raw
    finally:
        await _quit(user, other)
