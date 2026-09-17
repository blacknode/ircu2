"""CHATHISTORY: what was said, read back.

Phase 2 of doc/proposals/006-comunicaciones-unificadas.md.  The topology
is the identity one -- one ircd and one PostgreSQL (marker ``identity``)
-- because half of what is worth testing here needs accounts: a direct
message is stored only when both ends have identified, and a conversation
is read back by account.

The schema is created the way a real deployment creates it, by an
operator running /MODULE MIGRATION APPLY, and this file applies both
sets: identity's, because the tests log in, and history's.

See doc/readme.history.
"""

import asyncio
import random
import string

import pytest

from irc_client import IRCClient

pytestmark = pytest.mark.identity

#: A token unique to this run, put in every nickname and channel name.
#:
#: The store outlives the tests: PostgreSQL keeps what a previous run said,
#: and so does the account table, so a fixed name would read back somebody
#: else's messages and a fixed nickname would already be registered.
TAG = "".join(random.choice(string.ascii_lowercase) for _ in range(4))


#: Everything a client has to have negotiated for CHATHISTORY to work.
CAPS = [
    "batch",
    "message-tags",
    "server-time",
    "standard-replies",
    "draft/chathistory",
    # So a speaker can see its own message and compare it with the one it
    # is given back.
    "echo-message",
]


async def _oper(hub):
    client = IRCClient()
    await client.connect(hub["host"], hub["port"])
    await client.register(f"histoper{TAG}", f"histoper{TAG}", "History Oper")
    await client.send("OPER testoper operpass")
    await client.wait_for("381", timeout=5)
    return client


async def _collect(client, seconds):
    """Everything the server says for the next \a seconds."""
    out = []
    loop = asyncio.get_event_loop()
    deadline = loop.time() + seconds
    while loop.time() < deadline:
        try:
            msg = await client.recv(timeout=0.4)
        except (asyncio.TimeoutError, TimeoutError):
            continue
        if msg.params:
            out.append(msg.params[-1])
    return out


async def _apply(client, module):
    """Run the migrations for one module and wait until none are pending.

    STATUS is what is believed, not APPLY's own running commentary: the
    migrations run one connection at a time and the replies arrive as they
    finish, so the only thing that says the schema is there is the table
    itself.
    """
    await _collect(client, 0.5)
    await client.send(f"MODULE MIGRATION APPLY {module}")
    await _collect(client, 3)

    for _ in range(40):
        await client.send(f"MODULE MIGRATION STATUS {module}")
        lines = await _collect(client, 1.5)
        if (any("End of migrations" in line for line in lines)
                and not any("pending" in line for line in lines)):
            return
        await asyncio.sleep(0.5)

    raise AssertionError(f"migrations for {module} never finished")


# Which topology generation the schema was applied to, so that it is applied
# once per container and not once per test.  See topology_generation() in
# conftest.py for why this is not simply a session-scoped fixture.
_schema_generation = None


@pytest.fixture
async def schema(ircd_identity, topology_generation):
    """Create both schemas once, the way an operator would."""
    global _schema_generation

    if _schema_generation == topology_generation:
        return True

    client = await _oper(ircd_identity)
    try:
        await _apply(client, "identity")
        await _apply(client, "history")
    finally:
        await client.disconnect()

    _schema_generation = topology_generation
    return True


async def _connect(hub, nick, caps=CAPS):
    client = IRCClient()
    await client.connect(hub["host"], hub["port"])
    if caps:
        await client.negotiate_cap(caps)
    await client.register(nick, "histuser", "History Test")
    await _drain(client)
    return client


async def _drain(client, timeout=0.5):
    while True:
        try:
            await client.recv(timeout=timeout)
        except (asyncio.TimeoutError, TimeoutError):
            return


async def _chathistory(client, command, timeout=6.0):
    """Send a CHATHISTORY request and collect the batch it comes back in.

    Returns (batch_open, [messages inside], batch_close), or a single FAIL
    message when the request was refused.
    """
    await client.send(command)

    opened = None
    closed = None
    inside = []
    fail = None
    loop = asyncio.get_event_loop()
    deadline = loop.time() + timeout

    while loop.time() < deadline:
        try:
            msg = await client.recv(timeout=1.0)
        except (asyncio.TimeoutError, TimeoutError):
            if opened and closed:
                break
            continue

        if msg.command == "FAIL":
            fail = msg
            break
        if msg.command == "BATCH":
            if msg.params and msg.params[0].startswith("+"):
                opened = msg
            elif msg.params and msg.params[0].startswith("-"):
                closed = msg
                break
            continue
        if opened and "batch=" in (msg.tags or ""):
            inside.append(msg)

    if fail:
        return fail, [], None

    assert opened is not None, f"no batch came back for {command}"
    assert closed is not None, f"the batch was never closed for {command}"
    return opened, inside, closed


def _tag(msg, key):
    for part in (msg.tags or "").split(";"):
        if part.startswith(key + "="):
            return part[len(key) + 1:]
    return None


def _bodies(messages):
    return [m.params[-1] for m in messages]


# --------------------------------------------------------------------- #
# A channel                                                             #
# --------------------------------------------------------------------- #


async def test_channel_latest_comes_back_oldest_first(schema, ircd_identity):
    """What was said in a channel is read back in the order it was said."""
    speaker = await _connect(ircd_identity, f"hspeak1{TAG}")
    await speaker.send(f"JOIN #hist1{TAG}")
    await _drain(speaker)

    for i in range(5):
        await speaker.send(f"PRIVMSG #hist1{TAG} :line {i}")
    await asyncio.sleep(1.5)
    await _drain(speaker)

    reader = await _connect(ircd_identity, f"hread1{TAG}")
    await reader.send(f"JOIN #hist1{TAG}")
    await _drain(reader)

    opened, inside, _ = await _chathistory(reader, f"CHATHISTORY LATEST #hist1{TAG} * 10")

    assert opened.params[1] == "chathistory"
    assert opened.params[2] == f"#hist1{TAG}"
    assert _bodies(inside) == [f"line {i}" for i in range(5)]

    await speaker.disconnect()
    await reader.disconnect()


async def test_a_replayed_message_keeps_its_name_and_its_time(schema,
                                                              ircd_identity):
    """A stored message goes back out under the msgid and time it had.

    That is the whole point of storing them: the client is shown the same
    message everyone else saw, and can order it against what it already
    has.  A transcript stamped "now" would be a list of when somebody
    scrolled.
    """
    speaker = await _connect(ircd_identity, f"hspeak2{TAG}")
    await speaker.send(f"JOIN #hist2{TAG}")
    await _drain(speaker)

    await speaker.send(f"PRIVMSG #hist2{TAG} :remember me")
    live = await speaker.wait_for("PRIVMSG", timeout=5)
    assert live.params[-1] == "remember me"
    live_id = _tag(live, "msgid")
    live_time = _tag(live, "time")
    assert live_id, "a channel message should carry a msgid"
    await asyncio.sleep(1.5)
    await _drain(speaker)

    _, inside, _ = await _chathistory(speaker, f"CHATHISTORY LATEST #hist2{TAG} * 10")

    assert len(inside) == 1
    assert _tag(inside[0], "msgid") == live_id
    assert _tag(inside[0], "time") == live_time
    assert inside[0].prefix.startswith(f"hspeak2{TAG}!")

    await speaker.disconnect()


async def test_paging_through_a_channel(schema, ircd_identity):
    """BEFORE, AFTER and AROUND page around a message by name."""
    speaker = await _connect(ircd_identity, f"hspeak3{TAG}")
    await speaker.send(f"JOIN #hist3{TAG}")
    await _drain(speaker)

    for i in range(7):
        await speaker.send(f"PRIVMSG #hist3{TAG} :page {i}")
        await asyncio.sleep(0.05)
    await asyncio.sleep(1.5)
    await _drain(speaker)

    _, inside, _ = await _chathistory(speaker, f"CHATHISTORY LATEST #hist3{TAG} * 20")
    assert _bodies(inside) == [f"page {i}" for i in range(7)]

    middle = _tag(inside[3], "msgid")

    _, before, _ = await _chathistory(
        speaker, f"CHATHISTORY BEFORE #hist3{TAG} msgid={middle} 2")
    assert _bodies(before) == ["page 1", "page 2"]

    _, after, _ = await _chathistory(
        speaker, f"CHATHISTORY AFTER #hist3{TAG} msgid={middle} 2")
    assert _bodies(after) == ["page 4", "page 5"]

    _, around, _ = await _chathistory(
        speaker, f"CHATHISTORY AROUND #hist3{TAG} msgid={middle} 4")
    assert "page 3" in _bodies(around)
    assert len(around) == 4

    await speaker.disconnect()


async def test_between_reads_the_same_either_way_round(schema, ircd_identity):
    """BETWEEN's two points may arrive in either order."""
    speaker = await _connect(ircd_identity, f"hspeak4{TAG}")
    await speaker.send(f"JOIN #hist4{TAG}")
    await _drain(speaker)

    for i in range(5):
        await speaker.send(f"PRIVMSG #hist4{TAG} :span {i}")
        await asyncio.sleep(0.05)
    await asyncio.sleep(1.5)
    await _drain(speaker)

    _, inside, _ = await _chathistory(speaker, f"CHATHISTORY LATEST #hist4{TAG} * 20")
    first = _tag(inside[0], "msgid")
    last = _tag(inside[-1], "msgid")

    _, forwards, _ = await _chathistory(
        speaker, f"CHATHISTORY BETWEEN #hist4{TAG} msgid={first} msgid={last} 20")
    _, backwards, _ = await _chathistory(
        speaker, f"CHATHISTORY BETWEEN #hist4{TAG} msgid={last} msgid={first} 20")

    assert _bodies(forwards) == _bodies(backwards)
    # Both ends are exclusive.
    assert _bodies(forwards) == ["span 1", "span 2", "span 3"]

    await speaker.disconnect()


async def test_the_limit_is_the_servers(schema, ircd_identity):
    """A client asking for more than the maximum gets the maximum."""
    speaker = await _connect(ircd_identity, f"hspeak5{TAG}")
    await speaker.send(f"JOIN #hist5{TAG}")
    await _drain(speaker)

    for i in range(25):
        await speaker.send(f"PRIVMSG #hist5{TAG} :many {i}")
        await asyncio.sleep(0.02)
    await asyncio.sleep(2)
    await _drain(speaker)

    _, inside, _ = await _chathistory(speaker, f"CHATHISTORY LATEST #hist5{TAG} * 500")

    # HISTORY_MAX_LIMIT in tests/docker/ircd-identity.conf.
    assert len(inside) == 20

    await speaker.disconnect()


async def test_a_channel_you_are_not_on_is_refused(schema, ircd_identity):
    """A channel's history is exactly as private as the channel."""
    member = await _connect(ircd_identity, f"hspeak6{TAG}")
    await member.send(f"JOIN #hist6{TAG}")
    await _drain(member)
    await member.send(f"PRIVMSG #hist6{TAG} :members only")
    await asyncio.sleep(1.5)

    outsider = await _connect(ircd_identity, f"hout6{TAG}")
    failed, _, _ = await _chathistory(outsider,
                                      f"CHATHISTORY LATEST #hist6{TAG} * 10")

    assert failed.command == "FAIL"
    assert failed.params[0] == "CHATHISTORY"
    assert failed.params[1] == "INVALID_TARGET"

    await member.disconnect()
    await outsider.disconnect()


# --------------------------------------------------------------------- #
# What is not stored                                                    #
# --------------------------------------------------------------------- #


async def test_a_conversation_between_strangers_is_not_stored(schema,
                                                              ircd_identity):
    """A direct message with an unidentified end is not kept.

    There would be nobody it could later be shown to: the only durable
    handle on a person is the nickname they proved is theirs.
    """
    a = await _connect(ircd_identity, f"hanon1{TAG}")
    b = await _connect(ircd_identity, f"hanon2{TAG}")

    await a.send(f"PRIVMSG hanon2{TAG} :nobody will remember this")
    await asyncio.sleep(1.5)
    await _drain(a)

    failed, inside, _ = await _chathistory(
        a, f"CHATHISTORY LATEST hanon2{TAG} * 10")

    # Not identified, so there is no conversation to be had at all.
    assert failed.command == "FAIL"
    assert failed.params[1] == "INVALID_TARGET"

    await a.disconnect()
    await b.disconnect()


async def _register(hub, nick, address):
    """Bring up a client that has proved its nickname."""
    client = await _connect(hub, nick)
    await client.send(f"PRIVMSG NickServ :REGISTER {address} hunter2hunter2")

    loop = asyncio.get_event_loop()
    deadline = loop.time() + 15
    while loop.time() < deadline:
        try:
            msg = await client.recv(timeout=1.0)
        except (asyncio.TimeoutError, TimeoutError):
            continue
        if msg.command == "MODE" and "r" in msg.params[-1]:
            await _drain(client)
            return client

    raise AssertionError(f"{nick} never got +r")


async def test_a_conversation_is_read_back_by_both_ends(schema, ircd_identity):
    """A direct message between two identified people is kept, once.

    One row, not two: both ends read the same message back, and it is the
    same message they saw when it was sent.
    """
    a = await _register(ircd_identity, f"hconv1{TAG}", f"hconv1{TAG}@example.org")
    b = await _register(ircd_identity, f"hconv2{TAG}", f"hconv2{TAG}@example.org")

    await a.send(f"PRIVMSG hconv2{TAG} :are you there")
    await asyncio.sleep(0.3)
    await b.send(f"PRIVMSG hconv1{TAG} :I am here")
    await asyncio.sleep(2)
    await _drain(a)
    await _drain(b)

    opened, from_a, _ = await _chathistory(
        a, f"CHATHISTORY LATEST hconv2{TAG} * 10")
    assert opened.params[2] == f"hconv2{TAG}"
    assert _bodies(from_a) == ["are you there", "I am here"]

    _, from_b, _ = await _chathistory(
        b, f"CHATHISTORY LATEST hconv1{TAG} * 10")
    assert _bodies(from_b) == ["are you there", "I am here"]

    # The same messages, by the same names, from either end.
    assert ([_tag(m, "msgid") for m in from_a]
            == [_tag(m, "msgid") for m in from_b])

    await a.disconnect()
    await b.disconnect()


async def test_targets_lists_who_you_have_talked_to(schema, ircd_identity):
    a = await _register(ircd_identity, f"htarg1{TAG}", f"htarg1{TAG}@example.org")
    b = await _register(ircd_identity, f"htarg2{TAG}", f"htarg2{TAG}@example.org")

    await a.send(f"PRIVMSG htarg2{TAG} :hello there")
    await asyncio.sleep(2)
    await _drain(a)

    _, inside, _ = await _chathistory(a, "CHATHISTORY TARGETS * * 10")

    # CHATHISTORY TARGETS <target> <when>
    targets = [m.params[1] for m in inside if m.command == "CHATHISTORY"]
    assert f"htarg2{TAG}" in targets

    await a.disconnect()
    await b.disconnect()


async def test_a_third_party_cannot_read_a_conversation(schema, ircd_identity):
    """Asking about somebody else's conversation finds nothing.

    Not a refusal with a different shape, which would tell the asker the
    conversation exists: the query is by the pair of accounts, and the
    asker is not one of them, so there is simply nothing there.
    """
    a = await _register(ircd_identity, f"hsnoop1{TAG}", f"hsnoop1{TAG}@example.org")
    b = await _register(ircd_identity, f"hsnoop2{TAG}", f"hsnoop2{TAG}@example.org")
    c = await _register(ircd_identity, f"hsnoop3{TAG}", f"hsnoop3{TAG}@example.org")

    await a.send(f"PRIVMSG hsnoop2{TAG} :a secret")
    await asyncio.sleep(2)
    await _drain(c)

    _, inside, _ = await _chathistory(
        c, f"CHATHISTORY LATEST hsnoop1{TAG} * 10")
    assert inside == []

    _, targets, _ = await _chathistory(c, "CHATHISTORY TARGETS * * 10")
    assert targets == []

    await a.disconnect()
    await b.disconnect()
    await c.disconnect()


async def test_messages_to_a_service_are_never_stored(schema, ircd_identity):
    """That is where passwords go."""
    client = await _connect(ircd_identity, f"hsvc1{TAG}")
    await client.send("PRIVMSG NickServ :HELP")
    await asyncio.sleep(1.5)
    await _drain(client)

    failed, _, _ = await _chathistory(client,
                                      "CHATHISTORY LATEST NickServ * 10")

    # Unidentified, so refused before anything could be read -- and there
    # is nothing there either way.
    assert failed.command == "FAIL"

    await client.disconnect()


# --------------------------------------------------------------------- #
# Refusals                                                              #
# --------------------------------------------------------------------- #


async def test_an_unknown_subcommand_fails_by_name(schema, ircd_identity):
    client = await _connect(ircd_identity, f"hbad1{TAG}")
    failed, _, _ = await _chathistory(client, "CHATHISTORY SIDEWAYS #x * 10")

    assert failed.command == "FAIL"
    assert failed.params[1] == "UNKNOWN_COMMAND"
    assert failed.params[2] == "SIDEWAYS"

    await client.disconnect()


async def test_a_bad_selector_fails(schema, ircd_identity):
    client = await _connect(ircd_identity, f"hbad2{TAG}")
    await client.send(f"JOIN #hist7{TAG}")
    await _drain(client)

    failed, _, _ = await _chathistory(client,
                                      f"CHATHISTORY AFTER #hist7{TAG} yesterday 10")

    assert failed.command == "FAIL"
    assert failed.params[1] == "INVALID_PARAMS"

    await client.disconnect()


async def test_a_client_without_batch_is_told_in_words(schema, ircd_identity):
    """A client that cannot read a batch cannot be answered with one.

    It also has no standard-replies, so what it gets is a NOTICE carrying
    the same words -- the rule that a client which negotiated nothing sees
    only what it has always seen.
    """
    client = await _connect(ircd_identity, f"hplain{TAG}", caps=None)
    await client.send(f"JOIN #hist8{TAG}")
    await _drain(client)

    await client.send(f"CHATHISTORY LATEST #hist8{TAG} * 10")
    msg = await client.wait_for("NOTICE", timeout=5)

    assert "batch" in msg.params[-1]

    await client.disconnect()


# --------------------------------------------------------------------- #
# /HISTORY: the operator's side                                         #
# --------------------------------------------------------------------- #


async def _notices(client, command, seconds=6.0):
    """Send \a command and collect the text the server answers with."""
    await client.send(command)
    return await _collect(client, seconds)


async def test_history_needs_its_own_privilege(schema, ircd_identity):
    """Reading everyone's messages is not something being an oper grants."""
    client = await _connect(ircd_identity, f"hpriv{TAG}")
    await client.send("HISTORY STATUS")
    msg = await client.wait_for("481", timeout=5)

    assert "privileges" in msg.params[-1].lower()

    await client.disconnect()


async def test_history_status_counts_what_is_stored(schema, ircd_identity):
    speaker = await _connect(ircd_identity, f"hstat{TAG}")
    await speaker.send(f"JOIN #hstat{TAG}")
    await _drain(speaker)
    await speaker.send(f"PRIVMSG #hstat{TAG} :counted")
    await asyncio.sleep(1.5)

    oper = await _oper(ircd_identity)
    lines = await _notices(oper, "HISTORY STATUS")

    assert any("messages stored" in line for line in lines)
    assert any("keeping" in line for line in lines)

    await speaker.disconnect()
    await oper.disconnect()


async def test_export_and_forget_agree_on_what_is_yours(schema,
                                                        ircd_identity):
    """What a person is handed is what a person can have destroyed.

    Both are the same predicate -- everything the account sent and every
    direct message it received -- so the count the export writes is the
    count the delete removes.
    """
    a = await _register(ircd_identity, f"hgdpr1{TAG}", f"hgdpr1{TAG}@ex.org")
    b = await _register(ircd_identity, f"hgdpr2{TAG}", f"hgdpr2{TAG}@ex.org")

    await a.send(f"JOIN #hgdpr{TAG}")
    await _drain(a)
    await a.send(f"PRIVMSG #hgdpr{TAG} :in public")
    await a.send(f"PRIVMSG hgdpr2{TAG} :in private")
    await asyncio.sleep(0.3)
    await b.send(f"PRIVMSG hgdpr1{TAG} :answered")
    await asyncio.sleep(2)

    oper = await _oper(ircd_identity)

    lines = await _notices(oper, f"HISTORY STATUS hgdpr1{TAG}")
    assert any(f"hgdpr1{TAG} has 3 of" in line for line in lines), lines

    lines = await _notices(oper, f"HISTORY EXPORT hgdpr1{TAG}")
    assert any("exported 3 messages" in line for line in lines), lines
    assert any(".jsonl" in line for line in lines), lines

    lines = await _notices(oper, f"HISTORY FORGET hgdpr1{TAG}")
    assert any("forgot 3 messages" in line for line in lines), lines

    lines = await _notices(oper, f"HISTORY STATUS hgdpr1{TAG}")
    assert any(f"hgdpr1{TAG} has 0 of" in line for line in lines), lines

    # The other end's copy went with it: a direct message is one row.
    _, inside, _ = await _chathistory(
        b, f"CHATHISTORY LATEST hgdpr1{TAG} * 10")
    assert inside == []

    await a.disconnect()
    await b.disconnect()
    await oper.disconnect()


async def test_an_unknown_history_subcommand_fails(schema, ircd_identity):
    oper = await _oper(ircd_identity)
    lines = await _notices(oper, "HISTORY SIDEWAYS", seconds=3)

    assert any("STATUS, PURGE, EXPORT or FORGET" in line for line in lines)

    await oper.disconnect()


async def test_the_capability_advertises_the_limit(schema, ircd_identity):
    """A client learns the ceiling before it asks, not by being cut down."""
    client = IRCClient()
    await client.connect(ircd_identity["host"], ircd_identity["port"])
    await client.send("CAP LS 302")
    msg = await client.wait_for("CAP", timeout=5)

    assert "draft/chathistory=20" in msg.params[-1]

    await client.send("CAP END")
    await client.disconnect()
