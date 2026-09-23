"""Phases 3 and 4: what a conversation is, and what it looks like.

Threads, reactions, typing, REDACT, MARKREAD and rich text.  All of them
are built on the msgid the network already agrees on, and all of them are
in modules over the PostgreSQL the store topology brings up, which is
the topology they share (marker ``store``).

See doc/readme.history and doc/readme.richtext.
"""

import asyncio
import random
import string

import pytest

from irc_client import IRCClient

pytestmark = pytest.mark.store

#: Unique per run: the store outlives the tests and the accounts do too.
TAG = "".join(random.choice(string.ascii_lowercase) for _ in range(4))

CAPS = [
    "batch",
    "message-tags",
    "server-time",
    "standard-replies",
    "echo-message",
    "draft/chathistory",
    "draft/message-redaction",
    "draft/read-marker",
]

RICH_CAPS = CAPS + ["blacknode/richtext"]


async def _collect(client, seconds):
    out = []
    loop = asyncio.get_event_loop()
    deadline = loop.time() + seconds
    while loop.time() < deadline:
        try:
            out.append(await client.recv(timeout=0.4))
        except (asyncio.TimeoutError, TimeoutError):
            continue
    return out


async def _drain(client, timeout=0.5):
    while True:
        try:
            await client.recv(timeout=timeout)
        except (asyncio.TimeoutError, TimeoutError):
            return


async def _apply(client, module):
    await _drain(client)
    await client.send(f"MODULE MIGRATION APPLY {module}")
    await _collect(client, 3)

    for _ in range(40):
        await client.send(f"MODULE MIGRATION STATUS {module}")
        lines = [m.params[-1] for m in await _collect(client, 1.5) if m.params]
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
async def schema(ircd_store, topology_generation):
    global _schema_generation

    if _schema_generation == topology_generation:
        return True

    client = IRCClient()
    await client.connect(ircd_store["host"], ircd_store["port"])
    await client.register(f"cvoper{TAG}", f"cvoper{TAG}", "Conversation Oper")
    await client.send("OPER testoper operpass")
    await client.wait_for("381", timeout=5)
    try:
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
    await client.register(nick, "cvuser", "Conversation Test")
    await _drain(client)
    return client


async def _register(hub, services, nick, caps=CAPS):
    """A client whose nickname the network's services have vouched for.

    The account comes from an ACCOUNT out of the U:lined server, which is
    the only way one ever arrives (doc/readme.accounting).
    """
    client = await _connect(hub, nick, caps)
    await services.send_register(nick, account=nick)

    loop = asyncio.get_event_loop()
    deadline = loop.time() + 15
    while loop.time() < deadline:
        await client.send(f"MODE {nick}")
        msg = await client.wait_for("221", timeout=3.0)
        if "r" in msg.params[-1]:
            await _drain(client)
            return client
        await asyncio.sleep(0.3)

    raise AssertionError(f"{nick} never got an account")


def _tag(msg, key):
    for part in (msg.tags or "").split(";"):
        if part.startswith(key + "="):
            return part[len(key) + 1:]
    return None


async def _say(client, target, text, tags=""):
    """Send a message and return the msgid the server gave it."""
    prefix = f"@{tags} " if tags else ""
    await client.send(f"{prefix}PRIVMSG {target} :{text}")

    loop = asyncio.get_event_loop()
    deadline = loop.time() + 5
    while loop.time() < deadline:
        try:
            msg = await client.recv(timeout=1.0)
        except (asyncio.TimeoutError, TimeoutError):
            continue
        if msg.command == "PRIVMSG" and msg.params[-1] == text:
            return _tag(msg, "msgid")

    raise AssertionError(f"no echo for {text!r}")


async def _chathistory(client, command, timeout=6.0):
    await client.send(command)

    opened = closed = fail = None
    inside = []
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
        return fail, []

    assert opened is not None, f"no batch for {command}"
    return opened, inside


# --------------------------------------------------------------------- #
# Threads and reactions                                                 #
# --------------------------------------------------------------------- #


async def test_a_reply_keeps_what_it_replies_to(schema, ircd_store):
    a = await _connect(ircd_store, f"cva{TAG}")
    b = await _connect(ircd_store, f"cvb{TAG}")
    room = f"#cvt{TAG}"
    await a.send(f"JOIN {room}")
    await b.send(f"JOIN {room}")
    await _drain(a)
    await _drain(b)

    first = await _say(a, room, "the first message")
    assert first
    await _drain(b)

    await _say(b, room, "a reply", tags=f"+draft/reply={first}")
    await asyncio.sleep(1.5)
    await _drain(a)

    _, inside = await _chathistory(a, f"CHATHISTORY LATEST {room} * 10")
    bodies = [m.params[-1] for m in inside]

    assert bodies == ["the first message", "a reply"]
    assert _tag(inside[0], "+draft/reply") is None
    assert _tag(inside[1], "+draft/reply") == first

    await a.disconnect()
    await b.disconnect()


async def test_a_reaction_is_a_tagmsg_that_comes_back(schema, ircd_store):
    a = await _connect(ircd_store, f"cvra{TAG}")
    room = f"#cvr{TAG}"
    await a.send(f"JOIN {room}")
    await _drain(a)

    target = await _say(a, room, "react to me")
    await a.send(f"@+draft/react=yes;+draft/reply={target} TAGMSG {room}")
    await asyncio.sleep(1.5)
    await _drain(a)

    _, inside = await _chathistory(a, f"CHATHISTORY LATEST {room} * 10")

    reactions = [m for m in inside if m.command == "TAGMSG"]
    assert len(reactions) == 1
    assert _tag(reactions[0], "+draft/react") == "yes"
    assert _tag(reactions[0], "+draft/reply") == target

    await a.disconnect()


async def test_typing_is_relayed_and_not_kept(schema, ircd_store):
    """It is true for four seconds and then it is not."""
    a = await _connect(ircd_store, f"cvta{TAG}")
    b = await _connect(ircd_store, f"cvtb{TAG}")
    room = f"#cvty{TAG}"
    await a.send(f"JOIN {room}")
    await b.send(f"JOIN {room}")
    await _drain(a)
    await _drain(b)

    await _say(a, room, "something to keep")
    await _drain(b)

    await a.send(f"@+typing=active TAGMSG {room}")
    msg = await b.wait_for("TAGMSG", timeout=5)
    assert _tag(msg, "+typing") == "active"

    await asyncio.sleep(1.5)
    await _drain(a)

    _, inside = await _chathistory(a, f"CHATHISTORY LATEST {room} * 10")
    assert [m.command for m in inside] == ["PRIVMSG"]

    await a.disconnect()
    await b.disconnect()


# --------------------------------------------------------------------- #
# REDACT                                                                #
# --------------------------------------------------------------------- #


async def test_the_author_can_take_a_message_back(schema, ircd_store, store_services):
    a = await _register(ircd_store, store_services, f"cvda{TAG}")
    b = await _register(ircd_store, store_services, f"cvdb{TAG}")
    room = f"#cvd{TAG}"
    await a.send(f"JOIN {room}")
    await asyncio.sleep(0.3)
    await b.send(f"JOIN {room}")
    await _drain(a)
    await _drain(b)

    mid = await _say(b, room, "please forget this")
    await asyncio.sleep(1.5)
    await _drain(a)

    await b.send(f"REDACT {room} {mid} :mistake")

    seen = await a.wait_for("REDACT", timeout=6)
    assert seen.params[0] == room
    assert seen.params[1] == mid
    await _drain(b)

    _, inside = await _chathistory(a, f"CHATHISTORY LATEST {room} * 10")
    assert [m.params[-1] for m in inside] == []

    await a.disconnect()
    await b.disconnect()


async def test_somebody_else_cannot(schema, ircd_store, store_services):
    a = await _register(ircd_store, store_services, f"cvea{TAG}")
    b = await _register(ircd_store, store_services, f"cveb{TAG}")
    c = await _register(ircd_store, store_services, f"cvec{TAG}")
    room = f"#cve{TAG}"
    await a.send(f"JOIN {room}")
    await asyncio.sleep(0.3)
    await b.send(f"JOIN {room}")
    await c.send(f"JOIN {room}")
    await _drain(a)
    await _drain(b)
    await _drain(c)

    mid = await _say(b, room, "not yours to remove")
    await asyncio.sleep(1.5)
    await _drain(c)

    await c.send(f"REDACT {room} {mid}")
    msg = await c.wait_for("FAIL", timeout=6)

    assert msg.params[0] == "REDACT"
    assert msg.params[1] == "REDACT_FORBIDDEN"

    await a.disconnect()
    await b.disconnect()
    await c.disconnect()


async def test_a_channel_op_can(schema, ircd_store, store_services):
    """Moderating is not undoing, so an operator has no window."""
    a = await _register(ircd_store, store_services, f"cvfa{TAG}")
    b = await _register(ircd_store, store_services, f"cvfb{TAG}")
    room = f"#cvf{TAG}"

    await a.send(f"JOIN {room}")          # a creates it, so a has ops
    await asyncio.sleep(0.5)
    await b.send(f"JOIN {room}")
    await _drain(a)
    await _drain(b)

    mid = await _say(b, room, "moderate me")
    await asyncio.sleep(1.5)
    await _drain(a)

    await a.send(f"REDACT {room} {mid} :off topic")
    seen = await b.wait_for("REDACT", timeout=6)

    assert seen.params[1] == mid

    await a.disconnect()
    await b.disconnect()


async def test_an_unknown_message_is_refused(schema, ircd_store, store_services):
    a = await _register(ircd_store, store_services, f"cvga{TAG}")
    room = f"#cvg{TAG}"
    await a.send(f"JOIN {room}")
    await _drain(a)

    await a.send(f"REDACT {room} NOSUCHMESSAGE")
    msg = await a.wait_for("FAIL", timeout=6)

    assert msg.params[1] == "UNKNOWN_MSGID"

    await a.disconnect()


# --------------------------------------------------------------------- #
# MARKREAD                                                              #
# --------------------------------------------------------------------- #


async def test_a_read_marker_only_moves_forward(schema, ircd_store, store_services):
    a = await _register(ircd_store, store_services, f"cvma{TAG}")
    room = f"#cvm{TAG}"

    await a.send(f"MARKREAD {room}")
    msg = await a.wait_for("MARKREAD", timeout=5)
    assert msg.params[1] == "*"

    await a.send(f"MARKREAD {room} timestamp=2026-06-01T12:00:00.000Z")
    msg = await a.wait_for("MARKREAD", timeout=5)
    assert msg.params[1] == "timestamp=2026-06-01T12:00:00.000Z"

    # Backwards is not an error; the answer is where it already was.
    await a.send(f"MARKREAD {room} timestamp=2026-01-01T00:00:00.000Z")
    msg = await a.wait_for("MARKREAD", timeout=5)
    assert msg.params[1] == "timestamp=2026-06-01T12:00:00.000Z"

    await a.send(f"MARKREAD {room} timestamp=2026-07-01T00:00:00.000Z")
    msg = await a.wait_for("MARKREAD", timeout=5)
    assert msg.params[1] == "timestamp=2026-07-01T00:00:00.000Z"

    await a.disconnect()


async def test_read_markers_need_an_account(schema, ircd_store):
    """A person reads on their phone and expects their laptop to know."""
    a = await _connect(ircd_store, f"cvna{TAG}")

    await a.send(f"MARKREAD #cvn{TAG}")
    msg = await a.wait_for("FAIL", timeout=5)

    assert msg.params[0] == "MARKREAD"
    assert msg.params[1] == "NEED_REGISTRATION"

    await a.disconnect()


# --------------------------------------------------------------------- #
# Rich text                                                             #
# --------------------------------------------------------------------- #


async def test_markdown_reaches_one_half_and_plain_text_the_other(
        schema, ircd_store):
    """The rule the whole roadmap runs on, in one test."""
    sender = await _connect(ircd_store, f"cvrs{TAG}", RICH_CAPS)
    rich = await _connect(ircd_store, f"cvrr{TAG}", RICH_CAPS)
    plain = await _connect(ircd_store, f"cvrp{TAG}", caps=None)
    room = f"#cvrt{TAG}"

    for client in (sender, rich, plain):
        await client.send(f"JOIN {room}")
    await asyncio.sleep(0.8)
    for client in (sender, rich, plain):
        await _drain(client)

    body = "hello **world** see [the docs](https://example.org/d)"
    await sender.send(f"@+blacknode/format=markdown PRIVMSG {room} :{body}")

    got_rich = await rich.wait_for("PRIVMSG", timeout=5)
    got_plain = await plain.wait_for("PRIVMSG", timeout=5)

    assert got_rich.params[-1] == body
    assert _tag(got_rich, "+blacknode/format") == "markdown"

    assert got_plain.params[-1] == "hello world see the docs <https://example.org/d>"
    # A client that negotiated nothing sees the line it always saw.
    assert not got_plain.tags

    await sender.disconnect()
    await rich.disconnect()
    await plain.disconnect()


async def test_the_history_keeps_the_plain_text(schema, ircd_store):
    """One body per message, and it is the one everybody can read."""
    sender = await _connect(ircd_store, f"cvhs{TAG}", RICH_CAPS)
    room = f"#cvht{TAG}"
    await sender.send(f"JOIN {room}")
    await _drain(sender)

    await sender.send(f"@+blacknode/format=markdown PRIVMSG {room} "
                      f":kept as **plain**")
    await asyncio.sleep(2)
    await _drain(sender)

    _, inside = await _chathistory(sender, f"CHATHISTORY LATEST {room} * 10")

    assert [m.params[-1] for m in inside] == ["kept as plain"]

    await sender.disconnect()


@pytest.mark.parametrize("body,why", [
    ("<b>no html</b>", "html"),
    ("[click](javascript:alert(1))", "scheme"),
    ("***~~`****`~~*** deep", "depth"),
    ("**__**", "empty"),
])
async def test_what_rich_text_refuses(schema, ircd_store, body, why):
    # One nickname per case: they run in the same session against the same
    # server, and a shared one would be in use by the case before.
    sender = await _connect(ircd_store, f"cvx{why}{TAG}", RICH_CAPS)
    room = f"#cvx{why}{TAG}"
    await sender.send(f"JOIN {room}")
    await _drain(sender)

    await sender.send(f"@+blacknode/format=markdown PRIVMSG {room} :{body}")
    msg = await sender.wait_for("404", timeout=5)

    assert msg.params[-1], f"{why} should be refused with a reason"

    await sender.disconnect()


async def test_a_client_that_did_not_negotiate_sends_plain(schema,
                                                           ircd_store):
    """Marking a message without the capability agrees to nothing."""
    sender = await _connect(ircd_store, f"cvys{TAG}")
    reader = await _connect(ircd_store, f"cvyr{TAG}")
    room = f"#cvy{TAG}"
    await sender.send(f"JOIN {room}")
    await reader.send(f"JOIN {room}")
    await _drain(sender)
    await _drain(reader)

    await sender.send(f"@+blacknode/format=markdown PRIVMSG {room} "
                      f":left **alone**")
    msg = await reader.wait_for("PRIVMSG", timeout=5)

    assert msg.params[-1] == "left **alone**"

    await sender.disconnect()
    await reader.disconnect()
