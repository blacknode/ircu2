"""Identity: the freeze (+f), the guest rename, and what ACCOUNT refuses.

The model is doc/readme.accounting; how a user earns +r is
doc/readme.sasl.  What is tested here is the half of phase 1 that needs
no identity provider, which is the half a network runs whether or not it
has one:

* +f is a server's and a service bot's to set, never the user's, and
  while it is on, everything but identifying, talking to a service,
  changing nick and keeping the connection up gets ERR_FROZEN (987).
* +r clears +f in the same breath, because +r is the proof +f says is
  missing.
* Losing an identification means losing the nickname: ACCOUNT LOGOUT
  renames to guest-*.
* ACCOUNT refuses what it cannot answer, and says which refusal it is.
* With no provider registered the sasl capability is not advertised at
  all, and AUTHENTICATE is refused.
* No address ever arrives from another server.

A login end to end needs a real provider -- PostgreSQL, the identity
module and its migrations -- which the test topologies do not have yet;
see the note at the end of doc/readme.sasl.
"""

import asyncio
import re

import pytest

from irc_client import IRCClient, parse_mode_string

pytestmark = pytest.mark.single_server

SERVICES = "services.test.net"

#: guest-<8 characters of base 62>, as account_guest_nick() builds it.
GUEST_RE = re.compile(r"^guest-[0-9A-Za-z]{8}$")


@pytest.fixture
async def services(ircd_hub):
    """A U:lined P10 server linked to the hub.

    Nothing here needs a second ircd: what is being tested is one
    server's own behaviour, and the link exists only so that something
    other than the user can set +r and +f.
    """
    from p10_server import P10Server

    srv = P10Server(name=SERVICES, numeric=4, password="testpass")
    await srv.connect(ircd_hub["host"], ircd_hub["server_port"])
    await srv.handshake()
    yield srv
    await srv.disconnect()


async def _drain(client, timeout=0.5):
    """Read whatever is already queued and throw it away.

    Registration ends with a MODE +x that arrives after the MOTD, so a
    test that means "nothing happened after this point" has to start from
    a quiet connection or it will trip over the welcome.
    """
    while True:
        try:
            await client.recv(timeout=timeout)
        except asyncio.TimeoutError:
            return


async def _connect(hub, nick, username="testuser"):
    client = IRCClient()
    await client.connect(hub["host"], hub["port"])
    await client.register(nick, username, "Identity Test")
    await _drain(client)
    return client


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


async def _wait_mode(client, letter, sign, timeout=5.0):
    """Wait for a MODE on the client carrying ``sign``+``letter``."""
    loop = asyncio.get_running_loop()
    deadline = loop.time() + timeout
    while True:
        remaining = deadline - loop.time()
        if remaining <= 0:
            raise AssertionError(f"no MODE {sign}{letter} seen")
        msg = await client.wait_for("MODE", timeout=remaining)
        if parse_mode_string(msg.params[-1]).get(letter) == sign:
            return msg


async def _freeze(services, client, from_numnick=None):
    """Put +f on ``client`` from the server, or from one of its users."""
    await services.wait_for_user(client.nick)
    await services.send_user_mode(client.nick, "+f", from_numnick=from_numnick)
    return await _wait_mode(client, "f", "+")


async def _cap_ls(host, port):
    """The capability names a fresh connection is offered, values stripped."""
    client = IRCClient()
    await client.connect(host, port)
    try:
        await client.send("CAP LS 302")
        names = []
        while True:
            msg = await client.recv(timeout=5.0)
            if msg.command != "CAP" or len(msg.params) < 3:
                continue
            if msg.params[1] != "LS":
                continue
            names.extend(c.split("=", 1)[0] for c in msg.params[-1].split())
            if msg.params[2] != "*":
                return names
    finally:
        await client.disconnect()


# ---------------------------------------------------------------------------
# Who may freeze
# ---------------------------------------------------------------------------

async def test_user_cannot_set_plus_f(ircd_hub):
    """A user asking for +f on itself is put back the way it was."""
    hub = ircd_hub
    user = await _connect(hub, "id01")
    observer = await _connect(hub, "id01o")
    try:
        await user.send("MODE id01 +f")
        await user.assert_no_message("MODE", timeout=1.5)
        assert not _has(await _whois(observer, "id01"), "692")
    finally:
        await _quit(user, observer)


async def test_user_cannot_clear_its_own_freeze(ircd_hub, services):
    """The one mode a user must not be able to walk out of."""
    hub = ircd_hub
    user = await _connect(hub, "id02")
    observer = await _connect(hub, "id02o")
    try:
        await _freeze(services, user)

        await user.send("MODE id02 -f")
        # MODE is not one of the commands a frozen user may send, so this
        # does not even reach do_user_mode().
        await user.wait_for("987", timeout=5.0)
        assert _has(await _whois(observer, "id02"), "692")
    finally:
        await _quit(user, observer)


async def test_service_bot_may_freeze(ircd_hub, services):
    """+f is a +S bot's to set, like +r."""
    hub = ircd_hub
    user = await _connect(hub, "id03")
    observer = await _connect(hub, "id03o")
    try:
        bot = await services.introduce_user("id03ns", modes="+oikS")
        await _freeze(services, user, from_numnick=bot)
        assert _has(await _whois(observer, "id03"), "692")
    finally:
        await _quit(user, observer)


async def test_whois_shows_the_freeze(ircd_hub, services):
    """692, so it is visible from outside why that user is not answering."""
    hub = ircd_hub
    user = await _connect(hub, "id04")
    observer = await _connect(hub, "id04o")
    try:
        await _freeze(services, user)
        whois = await _whois(observer, "id04")
        frozen = [m for m in whois if m.command == "692"]
        assert frozen, f"expected 692 for a frozen user: {whois}"
        assert frozen[0].params[1] == "id04", frozen[0].raw
    finally:
        await _quit(user, observer)


# ---------------------------------------------------------------------------
# What a freeze blocks, and what it does not
# ---------------------------------------------------------------------------

async def test_frozen_user_cannot_join(ircd_hub, services):
    hub = ircd_hub
    user = await _connect(hub, "id10")
    try:
        await _freeze(services, user)
        await user.send("JOIN #frozen")
        msg = await user.wait_for("987", timeout=5.0)
        assert msg.params[1] == "id10", msg.raw
        await user.assert_no_message("JOIN", timeout=1.5)
    finally:
        await _quit(user)


async def test_frozen_user_cannot_message_a_user(ircd_hub, services):
    hub = ircd_hub
    user = await _connect(hub, "id11")
    other = await _connect(hub, "id11o")
    try:
        await _freeze(services, user)
        await user.send("PRIVMSG id11o :hello")
        await user.wait_for("987", timeout=5.0)
        await other.assert_no_message("PRIVMSG", timeout=1.5)
    finally:
        await _quit(user, other)


async def _wait_s2s(services, needle, timeout=5.0):
    """Read from the linked server until a line contains ``needle``."""
    loop = asyncio.get_running_loop()
    deadline = loop.time() + timeout
    while loop.time() < deadline:
        remaining = deadline - loop.time()
        line = await services._recv(timeout=max(remaining, 0.1))
        if needle in line:
            return line
    raise AssertionError(f"no S2S line carrying {needle!r}")


async def test_frozen_user_may_message_a_service_bot(ircd_hub, services):
    """The door that has to stay open: /msg NickServ identify."""
    hub = ircd_hub
    user = await _connect(hub, "id12")
    try:
        bot = await services.introduce_user("id12ns", modes="+oikS")
        await _freeze(services, user)

        await user.send("PRIVMSG id12ns :IDENTIFY someone@example.org secret")
        await user.assert_no_message("987", timeout=2.0)

        line = await _wait_s2s(services, f" P {bot} ")
        assert "IDENTIFY" in line, line
    finally:
        await _quit(user)


async def test_frozen_user_may_not_message_two_targets(ircd_hub, services):
    """A target list is refused rather than filtered down to the service."""
    hub = ircd_hub
    user = await _connect(hub, "id13")
    other = await _connect(hub, "id13o")
    try:
        await services.introduce_user("id13ns", modes="+oikS")
        await _freeze(services, user)

        await user.send("PRIVMSG id13ns,id13o :hello")
        await user.wait_for("987", timeout=5.0)
        await other.assert_no_message("PRIVMSG", timeout=1.5)
    finally:
        await _quit(user, other)


async def test_frozen_user_may_change_nick(ircd_hub, services):
    """One of the ways out of the freeze."""
    hub = ircd_hub
    user = await _connect(hub, "id14")
    try:
        await _freeze(services, user)
        await user.send("NICK id14b")
        msg = await user.wait_for("NICK", timeout=5.0)
        assert msg.params[0] == "id14b", msg.raw
    finally:
        await _quit(user)


async def test_frozen_user_may_ping(ircd_hub, services):
    """What keeps the connection up is never blocked."""
    hub = ircd_hub
    user = await _connect(hub, "id15")
    try:
        await _freeze(services, user)
        await user.send("PING :alive")
        await user.wait_for("PONG", timeout=5.0)
    finally:
        await _quit(user)


# ---------------------------------------------------------------------------
# +r and +f
# ---------------------------------------------------------------------------

async def test_granting_plus_r_clears_the_freeze(ircd_hub, services):
    """The core does this itself, so every way in lifts the freeze."""
    hub = ircd_hub
    user = await _connect(hub, "id20")
    observer = await _connect(hub, "id20o")
    try:
        await _freeze(services, user)
        await services.send_register("id20")
        await _wait_mode(user, "f", "-")

        whois = await _whois(observer, "id20")
        assert _has(whois, "307"), whois
        assert not _has(whois, "692"), whois

        # And it can act again.
        await user.send("JOIN #thawed")
        msg = await user.wait_for("JOIN", timeout=5.0)
        assert msg.params[0] == "#thawed", msg.raw
    finally:
        await _quit(user, observer)


async def test_freeze_survives_a_case_only_nick_change(ircd_hub, services):
    """Different capitals are the same nickname to the whole network."""
    hub = ircd_hub
    user = await _connect(hub, "id21")
    observer = await _connect(hub, "id21o")
    try:
        await _freeze(services, user)
        await user.send("NICK ID21")
        await user.wait_for("NICK", timeout=5.0)
        assert _has(await _whois(observer, "ID21"), "692")
    finally:
        await _quit(user, observer)


# ---------------------------------------------------------------------------
# Losing an identification means losing the nickname
# ---------------------------------------------------------------------------

async def test_account_logout_renames_to_a_guest(ircd_hub, services):
    """A client still called maria without +r is what an onlooker cannot
    tell apart from an impostor, so the nickname goes back with the
    session."""
    hub = ircd_hub
    user = await _connect(hub, "id30")
    try:
        await services.wait_for_user("id30")
        await services.send_register("id30")
        await _wait_mode(user, "r", "+")

        await user.send("ACCOUNT LOGOUT")
        await _wait_mode(user, "r", "-")

        renamed = await user.wait_for("NICK", timeout=5.0)
        assert GUEST_RE.match(renamed.params[0]), renamed.raw

        logged_out = await user.wait_for("901", timeout=5.0)
        assert logged_out.params[1].startswith(renamed.params[0] + "!"), \
            logged_out.raw
    finally:
        await _quit(user)


async def test_account_logout_without_an_account(ircd_hub):
    hub = ircd_hub
    user = await _connect(hub, "id31")
    try:
        await user.send("ACCOUNT LOGOUT")
        await user.wait_for("986", timeout=5.0)
        await user.assert_no_message("NICK", timeout=1.5)
    finally:
        await _quit(user)


# ---------------------------------------------------------------------------
# What ACCOUNT refuses
# ---------------------------------------------------------------------------

async def test_account_list_needs_an_identification(ircd_hub, services):
    """The input is the address this client proved was its own.  Even a
    user the network has marked +r has no address here: the address is
    set by whoever checked the credential, and nobody did."""
    hub = ircd_hub
    user = await _connect(hub, "id40")
    try:
        await user.send("ACCOUNT LIST")
        await user.wait_for("986", timeout=5.0)

        await services.wait_for_user("id40")
        await services.send_register("id40")
        await _wait_mode(user, "r", "+")

        await user.send("ACCOUNT LIST")
        await user.wait_for("986", timeout=5.0)
    finally:
        await _quit(user)


async def test_account_without_a_subcommand(ircd_hub):
    hub = ircd_hub
    user = await _connect(hub, "id41")
    try:
        await user.send("ACCOUNT")
        msg = await user.wait_for("461", timeout=5.0)
        assert msg.params[1] == "ACCOUNT", msg.raw
        assert len(msg.params) == 3, msg.raw
    finally:
        await _quit(user)


async def test_account_with_an_unknown_subcommand(ircd_hub):
    """421, not a failed authentication: nothing was attempted.  The
    subcommand alone is the parameter -- a numeric takes one token, and
    "ACCOUNT FROB" would be two."""
    hub = ircd_hub
    user = await _connect(hub, "id42")
    try:
        await user.send("ACCOUNT FROB")
        msg = await user.wait_for("421", timeout=5.0)
        assert msg.params[1] == "FROB", msg.raw
        assert len(msg.params) == 3, msg.raw
    finally:
        await _quit(user)


async def test_account_login_needs_both_parameters(ircd_hub):
    hub = ircd_hub
    user = await _connect(hub, "id43")
    try:
        await user.send("ACCOUNT LOGIN someone@example.org")
        msg = await user.wait_for("461", timeout=5.0)
        assert msg.params[1] == "ACCOUNT", msg.raw
        assert len(msg.params) == 3, msg.raw
    finally:
        await _quit(user)


async def test_account_login_without_a_provider(ircd_hub):
    """983 with the reason, and no hint that it might have worked."""
    hub = ircd_hub
    user = await _connect(hub, "id44")
    try:
        await user.send("ACCOUNT LOGIN someone@example.org secret")
        msg = await user.wait_for("983", timeout=5.0)
        assert "not available" in msg.params[-1], msg.raw
        await user.assert_no_message("900", timeout=1.5)
    finally:
        await _quit(user)


# ---------------------------------------------------------------------------
# SASL with nobody to answer
# ---------------------------------------------------------------------------

async def test_sasl_is_not_advertised_without_a_provider(ircd_hub):
    """A client that negotiated SASL against a server which cannot
    authenticate anybody would find out after sending its password."""
    hub = ircd_hub
    names = await _cap_ls(hub["host"], hub["port"])
    assert names, "CAP LS returned nothing"
    assert "sasl" not in names, names


async def test_authenticate_without_the_capability(ircd_hub):
    hub = ircd_hub
    user = await _connect(hub, "id50")
    try:
        await user.send("AUTHENTICATE PLAIN")
        await user.wait_for("904", timeout=5.0)
        await user.assert_no_message("AUTHENTICATE", timeout=1.5)
    finally:
        await _quit(user)


# ---------------------------------------------------------------------------
# The address is local and only local
# ---------------------------------------------------------------------------

async def test_no_address_arrives_from_a_burst(ircd_hub, services):
    """A +r user from another server carries no address, and none is
    invented for it: 307 says it is identified, 691 says with what, and
    only this server's own users can have the second."""
    hub = ircd_hub
    observer = await _connect(hub, "id60o")
    try:
        await services.introduce_user("id60r", modes="+ir")
        await asyncio.sleep(0.3)

        whois = await _whois(observer, "id60r")
        assert _has(whois, "307"), whois
        assert not _has(whois, "691"), whois
    finally:
        await _quit(observer)


async def test_no_address_for_a_user_nobody_checked(ircd_hub, services):
    """Not even to itself: +r from a server says the nickname is proved,
    not what address proved it."""
    hub = ircd_hub
    user = await _connect(hub, "id61")
    try:
        await services.wait_for_user("id61")
        await services.send_register("id61")
        await _wait_mode(user, "r", "+")

        whois = await _whois(user, "id61")
        assert _has(whois, "307"), whois
        assert not _has(whois, "691"), whois
    finally:
        await _quit(user)
