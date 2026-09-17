"""Identity with a real provider: the identity module over PostgreSQL.

tests/identity/ covers the half of phase 1 that needs no provider.  This
is the other half -- a login that goes all the way to the store and back
-- and it is the only place the schema, the Argon2 on a worker and the
advisory locks are exercised together.

The topology is one ircd and one PostgreSQL (docker-compose.yml, marker
``identity``).  There is no Redis: the cache is never the truth, so a
server without one answers exactly the same and one container fewer has
to come up.  See doc/readme.sasl.

The schema is created once per session by an operator running
/MODULE MIGRATION APPLY identity, which is how a real deployment does it
and therefore worth doing the same way here.
"""

import asyncio
import re

import pytest

from irc_client import IRCClient

pytestmark = pytest.mark.identity

#: guest-<8 characters of base 62>, as account_guest_nick() builds it.
GUEST_RE = re.compile(r"^guest-[0-9A-Za-z]{8}$")

#: "max_accounts" in the Service{} block of tests/docker/ircd-identity.conf.
MAX_ACCOUNTS = 2

#: "grace_period" there, in seconds.
GRACE = 10


async def _connect(hub, nick, username="testuser"):
    client = IRCClient()
    await client.connect(hub["host"], hub["port"])
    await client.register(nick, username, "Identity DB Test")
    await _drain(client)
    return client


async def _drain(client, timeout=0.5):
    while True:
        try:
            await client.recv(timeout=timeout)
        except asyncio.TimeoutError:
            return


async def _quit(*clients):
    for c in clients:
        try:
            await c.send("QUIT :cleanup")
        except Exception:
            pass
        await c.disconnect()


async def _oper(client):
    await client.send("OPER testoper operpass")
    await client.wait_for("381", timeout=10.0)


async def _collect(client, until, timeout=20.0):
    """Read until a message whose command is in ``until`` arrives.

    Everything read on the way is returned with it, because an answer that
    came from the database is preceded by the NICK and MODE the grant
    produced and a test usually wants to see both.
    """
    loop = asyncio.get_running_loop()
    deadline = loop.time() + timeout
    seen = []
    while True:
        remaining = deadline - loop.time()
        if remaining <= 0:
            raise AssertionError(f"none of {until} arrived; saw {seen}")
        msg = await client.recv(timeout=remaining)
        seen.append(msg)
        if msg.command in until:
            return seen


async def _ns(client, line, until, timeout=20.0):
    """Send a line to NickServ and read until one of ``until`` arrives."""
    await client.send(f"PRIVMSG NickServ :{line}")
    return await _collect(client, until, timeout=timeout)


def _notices(msgs):
    return [m.params[-1] for m in msgs if m.command == "NOTICE"]


async def _read_notices(client, timeout=10.0):
    """Every NOTICE that arrives until the stream goes quiet."""
    lines = []
    loop = asyncio.get_running_loop()
    deadline = loop.time() + timeout
    while loop.time() < deadline:
        try:
            msg = await client.recv(timeout=1.0)
        except asyncio.TimeoutError:
            if lines:
                return lines
            continue
        if msg.command == "NOTICE":
            lines.append(msg.params[-1])
    return lines


def _numeric(msgs, code):
    for m in msgs:
        if m.command == code:
            return m
    return None


# Which topology generation the schema was applied to, so that it is applied
# once per container and not once per test.  See topology_generation() in
# conftest.py for why this is not simply a session-scoped fixture.
_schema_generation = None


@pytest.fixture
async def schema(ircd_identity, topology_generation):
    """Apply the identity module's migrations, once per container.

    /MODULE MIGRATION APPLY is what an operator runs, so running it here
    covers the path a deployment takes rather than reaching around it with
    psql.
    """
    global _schema_generation

    if _schema_generation != topology_generation:
        client = IRCClient()
        await client.connect(ircd_identity["host"], ircd_identity["port"])
        try:
            await client.register("migrator", "oper", "Migrator")
            await _drain(client)
            await _oper(client)

            # Applying is asynchronous and idempotent: "Nothing to apply" on
            # a container that has already been through this.  Either way the
            # question that matters is what STATUS says afterwards, so ask
            # that until nothing is pending rather than trying to read the
            # running commentary.
            await client.send("MODULE MIGRATION APPLY identity")
            await _drain(client, timeout=2.0)

            deadline = asyncio.get_running_loop().time() + 90.0
            status = ""
            while asyncio.get_running_loop().time() < deadline:
                await client.send("MODULE MIGRATION STATUS identity")
                lines = await _read_notices(client, timeout=20.0)
                status = " ".join(lines)
                if status and "pending" not in status:
                    break
                await asyncio.sleep(1.0)

            assert status, "MODULE MIGRATION STATUS said nothing"
            assert "pending" not in status, status
            assert "applied" in status, status
        finally:
            await _quit(client)

        _schema_generation = topology_generation

    yield


@pytest.fixture
async def address(ircd_identity, schema):
    """A registered address nobody else is using, and its password.

    Registered through NickServ, which is the only way an account comes
    into being: there is no back door that writes the row directly,
    because the locking and the Argon2 are part of what is being tested.
    """
    import uuid

    local = "u" + uuid.uuid4().hex[:8]
    email = f"{local}@example.org"
    password = "pass-" + uuid.uuid4().hex[:8]
    nick = local

    client = await _connect(ircd_identity, nick)
    try:
        msgs = await _ns(client, f"REGISTER {email} {password}", {"MODE"})
        assert any("is yours" in n for n in _notices(msgs)), _notices(msgs)
    finally:
        await _quit(client)

    return {"email": email, "password": password, "nick": nick}


# ---------------------------------------------------------------------------
# The capability, once somebody can answer for it
# ---------------------------------------------------------------------------

async def test_sasl_is_advertised_with_a_provider(ircd_identity, schema):
    """The mirror of the test in tests/identity/: with a provider loaded
    the capability is offered, and its value is the mechanism list."""
    client = IRCClient()
    await client.connect(ircd_identity["host"], ircd_identity["port"])
    try:
        await client.send("CAP LS 302")
        caps = []
        while True:
            msg = await client.recv(timeout=10.0)
            if msg.command != "CAP" or len(msg.params) < 3 or msg.params[1] != "LS":
                continue
            caps.extend(msg.params[-1].split())
            if msg.params[2] != "*":
                break
    finally:
        await client.disconnect()

    sasl = [c for c in caps if c == "sasl" or c.startswith("sasl=")]
    assert sasl, caps
    mechs = sasl[0].split("=", 1)[1].split(",") if "=" in sasl[0] else []
    assert "PLAIN" in mechs, sasl
    assert "EXTERNAL" in mechs, sasl
    # Sorted by name, so the value does not depend on module load order.
    assert mechs == sorted(mechs), sasl


# ---------------------------------------------------------------------------
# Registering
# ---------------------------------------------------------------------------

async def test_register_takes_the_nickname_in_use(ircd_identity, schema):
    """An account is a nickname, and the one it registers is the one the
    client is wearing."""
    import uuid

    nick = "reg" + uuid.uuid4().hex[:6]
    email = f"{nick}@example.org"
    client = await _connect(ircd_identity, nick)
    try:
        msgs = await _ns(client, f"REGISTER {email} secreto", {"MODE"})
        assert any(f"{nick} is yours" in n for n in _notices(msgs)), _notices(msgs)

        # And identified on the spot: the password was just set.
        mode = _numeric(msgs, "MODE")
        assert mode and "r" in mode.params[-1], mode.raw

        whois = []
        await client.send(f"WHOIS {nick}")
        whois = await _collect(client, {"318"})
        assert _numeric(whois, "307"), [m.raw for m in whois]
        email_line = _numeric(whois, "691")
        assert email_line, [m.raw for m in whois]
        assert email_line.params[2] == email, email_line.raw
    finally:
        await _quit(client)


async def test_a_nickname_somebody_registered_cannot_be_taken(ircd_identity,
                                                              address):
    import uuid

    other = "oth" + uuid.uuid4().hex[:6]
    client = await _connect(ircd_identity, address["nick"] + "x")
    try:
        # REGISTER takes the nickname in use, so taking somebody else's
        # means wearing it -- which freezes the client.  That is the point
        # of the freeze, and it still leaves the one door open: a PRIVMSG
        # to a +S bot.  So the warning comes first and the refusal after.
        await client.send(f"NICK {address['nick']}")
        warned = await _collect(client, {"NOTICE"}, timeout=20.0)
        assert any("registered" in n for n in _notices(warned)), \
            _notices(warned)
        await _drain(client)

        msgs = await _ns(client,
                         f"REGISTER {other}@example.org secreto", {"NOTICE"})
        assert any("registered already" in n for n in _notices(msgs)), \
            _notices(msgs)
    finally:
        await _quit(client)


async def test_the_account_limit_is_enforced(ircd_identity, address):
    """max_accounts in the Service{} block, counted and applied inside the
    transaction that inserts."""
    import uuid

    client = await _connect(ircd_identity, "lim" + uuid.uuid4().hex[:6])
    try:
        # One is already registered by the fixture; take the rest.
        for _ in range(MAX_ACCOUNTS - 1):
            nick = "lim" + uuid.uuid4().hex[:6]
            await client.send(f"NICK {nick}")
            await _collect(client, {"NICK"})
            msgs = await _ns(client,
                             f"REGISTER {address['email']} {address['password']}",
                             {"MODE", "NOTICE"})
            assert any("is yours" in n for n in _notices(msgs)), _notices(msgs)

        nick = "lim" + uuid.uuid4().hex[:6]
        await client.send(f"NICK {nick}")
        await _collect(client, {"NICK"})
        msgs = await _ns(client,
                         f"REGISTER {address['email']} {address['password']}",
                         {"NOTICE"})
        assert any("as many accounts as it may" in n for n in _notices(msgs)), \
            _notices(msgs)
    finally:
        await _quit(client)


async def test_a_second_account_needs_the_address_password(ircd_identity,
                                                           address):
    """Taking another nickname for an address is as much an act of its
    holder as changing the password is."""
    import uuid

    client = await _connect(ircd_identity, "sec" + uuid.uuid4().hex[:6])
    try:
        msgs = await _ns(client,
                         f"REGISTER {address['email']} not-the-password",
                         {"NOTICE"})
        assert any("password is wrong" in n for n in _notices(msgs)), \
            _notices(msgs)
    finally:
        await _quit(client)


# ---------------------------------------------------------------------------
# Identifying, by all three doors
# ---------------------------------------------------------------------------

async def test_identify_through_nickserv(ircd_identity, address):
    client = await _connect(ircd_identity, address["nick"] + "z")
    try:
        msgs = await _ns(
            client,
            f"IDENTIFY {address['email']} {address['password']} {address['nick']}",
            {"900"})
        logged = _numeric(msgs, "900")
        assert logged and logged.params[2] == address["nick"], logged.raw
    finally:
        await _quit(client)


async def test_identify_through_account_login(ircd_identity, address):
    client = await _connect(ircd_identity, "acc" + address["nick"][:5])
    try:
        await client.send(
            f"ACCOUNT LOGIN {address['email']} {address['password']} "
            f"{address['nick']}")
        msgs = await _collect(client, {"900", "983"})
        logged = _numeric(msgs, "900")
        assert logged, [m.raw for m in msgs]
        assert logged.params[2] == address["nick"], logged.raw
    finally:
        await _quit(client)


async def test_identify_through_sasl_during_registration(ircd_identity,
                                                         address):
    """The one that has to work before the client is a user: the nickname
    is taken at once and the 900/903 arrive before CAP END."""
    import base64

    plain = base64.b64encode(
        f"{address['nick']}\0{address['email']}\0{address['password']}".encode()
    ).decode()

    client = IRCClient()
    await client.connect(ircd_identity["host"], ircd_identity["port"])
    try:
        await client.send("CAP LS 302")
        await client.send("NICK sasl" + address["nick"][:4])
        await client.send("USER s 0 * :SASL")
        await client.send("CAP REQ :sasl")
        await _collect(client, {"CAP"})

        await client.send("AUTHENTICATE PLAIN")
        await _collect(client, {"AUTHENTICATE"})

        await client.send(f"AUTHENTICATE {plain}")
        msgs = await _collect(client, {"903", "904"})
        assert _numeric(msgs, "903"), [m.raw for m in msgs]

        logged = _numeric(msgs, "900")
        assert logged and logged.params[2] == address["nick"], \
            [m.raw for m in msgs]

        # Renamed before registration finished, not after.
        renamed = [m for m in msgs if m.command == "NICK"]
        assert renamed, [m.raw for m in msgs]

        await client.send("CAP END")
        welcome = await _collect(client, {"001"})
        assert welcome[-1].params[0] == address["nick"], welcome[-1].raw
    finally:
        await _quit(client)


async def test_a_wrong_password_is_the_same_answer_as_no_such_address(
        ircd_identity, address):
    """Which accounts an address holds is not something a wrong password
    gets to find out, so the two failures are one answer."""
    client = await _connect(ircd_identity, "wrg" + address["nick"][:5])
    try:
        await client.send(f"ACCOUNT LOGIN {address['email']} wrong-password")
        bad_password = await _collect(client, {"983", "900"})

        await client.send("ACCOUNT LOGIN nobody-at-all@example.org whatever")
        no_address = await _collect(client, {"983", "900"})

        first = _numeric(bad_password, "983")
        second = _numeric(no_address, "983")
        assert first and second, [m.raw for m in bad_password + no_address]
        assert first.params[-1] == second.params[-1], (first.raw, second.raw)
    finally:
        await _quit(client)


# ---------------------------------------------------------------------------
# The listing
# ---------------------------------------------------------------------------

async def test_account_list_shows_what_the_address_holds(ircd_identity,
                                                         address):
    client = await _connect(ircd_identity, "lst" + address["nick"][:5])
    try:
        await client.send(
            f"ACCOUNT LOGIN {address['email']} {address['password']} "
            f"{address['nick']}")
        await _collect(client, {"900", "983"})
        await _drain(client)

        await client.send("ACCOUNT LIST")
        msgs = await _collect(client, {"985"})
        entries = [m for m in msgs if m.command == "984"]
        assert entries, [m.raw for m in msgs]

        by_nick = {m.params[1]: m.params[2] for m in entries}
        assert address["nick"] in by_nick, by_nick
        # The one in use carries "*", and the first account an address
        # registers is its default.
        assert "*" in by_nick[address["nick"]], by_nick
        assert "d" in by_nick[address["nick"]], by_nick
    finally:
        await _quit(client)


# ---------------------------------------------------------------------------
# The grace period, with a store that can answer
# ---------------------------------------------------------------------------

async def test_a_registered_nickname_freezes_a_stranger(ircd_identity,
                                                        address):
    """The whole of proposal 007 sections 5 to 7, end to end: the lookup
    reaches PostgreSQL, NickServ freezes the client, and the deadline
    renames it."""
    client = await _connect(ircd_identity, "str" + address["nick"][:5])
    try:
        await client.send(f"NICK {address['nick']}")
        # The mode comes first and the warning after it, so reading until
        # the warning gets both.
        msgs = await _collect(client, {"NOTICE"}, timeout=20.0)
        mode = _numeric(msgs, "MODE")
        assert mode and "+f" in mode.params[-1], [m.raw for m in msgs]
        assert any("registered" in n for n in _notices(msgs)), _notices(msgs)

        await client.send("JOIN #nope")
        blocked = await _collect(client, {"987", "JOIN"})
        assert _numeric(blocked, "987"), [m.raw for m in blocked]

        renamed = await _collect(client, {"NICK"}, timeout=GRACE + 20.0)
        nick_msg = _numeric(renamed, "NICK")
        assert GUEST_RE.match(nick_msg.params[0]), nick_msg.raw
    finally:
        await _quit(client)


async def test_identifying_lifts_the_freeze(ircd_identity, address):
    client = await _connect(ircd_identity, "thw" + address["nick"][:5])
    try:
        await client.send(f"NICK {address['nick']}")
        await _collect(client, {"MODE"}, timeout=20.0)
        await _drain(client)

        # No account named: frozen means "prove this nickname", so the one
        # in use is what IDENTIFY means.
        msgs = await _ns(client,
                         f"IDENTIFY {address['email']} {address['password']}",
                         {"900"})
        mode = _numeric(msgs, "MODE")
        assert mode and "-f" in mode.params[-1], mode.raw
        assert "+r" in mode.params[-1], mode.raw

        await client.send("JOIN #thawed")
        joined = await _collect(client, {"JOIN", "987"})
        assert _numeric(joined, "JOIN"), [m.raw for m in joined]
    finally:
        await _quit(client)


async def test_an_unregistered_nickname_is_left_alone(ircd_identity, schema):
    """The lookup answers "free" and nothing happens -- which is what most
    of the traffic on a real network looks like."""
    import uuid

    client = await _connect(ircd_identity, "fre" + uuid.uuid4().hex[:6])
    try:
        await client.send("JOIN #free")
        joined = await _collect(client, {"JOIN", "987"})
        assert _numeric(joined, "JOIN"), [m.raw for m in joined]
    finally:
        await _quit(client)


# ---------------------------------------------------------------------------
# Changing the password, and giving the nickname up
# ---------------------------------------------------------------------------

async def test_password_change_invalidates_the_old_one(ircd_identity, address):
    new_password = address["password"] + "-new"

    client = await _connect(ircd_identity, "chg" + address["nick"][:5])
    try:
        await client.send(
            f"ACCOUNT LOGIN {address['email']} {address['password']} "
            f"{address['nick']}")
        await _collect(client, {"900", "983"})
        await _drain(client)

        msgs = await _ns(client,
                         f"PASSWORD {address['password']} {new_password}",
                         {"NOTICE"}, timeout=30.0)
        assert any("password has been changed" in n for n in _notices(msgs)), \
            _notices(msgs)
    finally:
        await _quit(client)

    client = await _connect(ircd_identity, "chk" + address["nick"][:5])
    try:
        await client.send(
            f"ACCOUNT LOGIN {address['email']} {address['password']} "
            f"{address['nick']}")
        old = await _collect(client, {"983", "900"})
        assert _numeric(old, "983"), [m.raw for m in old]

        await client.send(
            f"ACCOUNT LOGIN {address['email']} {new_password} "
            f"{address['nick']}")
        new = await _collect(client, {"983", "900"})
        assert _numeric(new, "900"), [m.raw for m in new]
    finally:
        await _quit(client)


async def test_drop_gives_the_nickname_back(ircd_identity, address):
    client = await _connect(ircd_identity, "drp" + address["nick"][:5])
    try:
        await client.send(
            f"ACCOUNT LOGIN {address['email']} {address['password']} "
            f"{address['nick']}")
        await _collect(client, {"900", "983"})
        await _drain(client)

        msgs = await _ns(client, f"DROP {address['password']}",
                         {"NICK"}, timeout=30.0)
        assert any("no longer registered" in n for n in _notices(msgs)), \
            _notices(msgs)

        # Dropping ends the identification, so the nickname goes back.
        nick_msg = _numeric(msgs, "NICK")
        assert GUEST_RE.match(nick_msg.params[0]), nick_msg.raw
    finally:
        await _quit(client)

    # And it is free again: somebody else may now wear it unfrozen.
    client = await _connect(ircd_identity, "aft" + address["nick"][:5])
    try:
        await client.send(f"NICK {address['nick']}")
        await _collect(client, {"NICK"})
        await client.send("JOIN #free-again")
        joined = await _collect(client, {"JOIN", "987"}, timeout=20.0)
        assert _numeric(joined, "JOIN"), [m.raw for m in joined]
    finally:
        await _quit(client)
