"""SEARCH and EDIT: finding what was said, and changing it.

Two additions to the same store, written together because they are two
halves of the same argument: neither is an IRCv3 specification, and both
live in the history module because neither can be done by a server that
kept nothing.  You cannot search what nobody stored and you cannot edit
it either.

The topology is the store one, for the reason the rest of history/ is:
an edit is allowed only to the author, and the only durable name for an
author is the account they proved.

See doc/readme.history.
"""

import asyncio

import pytest

from history.test_history import (  # noqa: F401  (pytest fixtures)
    TAG,
    _bodies,
    _chathistory,
    _connect,
    _drain,
    _register,
    _tag,
    schema,
)

pytestmark = pytest.mark.store

#: What these tests negotiate on top of what test_history.py asks for.
CAPS = [
    "batch",
    "message-tags",
    "server-time",
    "standard-replies",
    "draft/chathistory",
    "echo-message",
    "blacknode/search",
    "blacknode/message-edit",
]


async def _client(hub, nick):
    return await _connect(hub, nick, caps=CAPS)


async def _identified(hub, services, nick):
    """A client the services have vouched for, with our capabilities.

    The account comes from an ACCOUNT out of the U:lined server, which is
    the only way one ever arrives (doc/readme.accounting).
    """
    client = await _client(hub, nick)
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


async def _wait_for(client, *commands, timeout=10.0):
    """The next message with one of these commands, or None."""
    loop = asyncio.get_event_loop()
    deadline = loop.time() + timeout

    while loop.time() < deadline:
        try:
            msg = await client.recv(timeout=1.0)
        except (asyncio.TimeoutError, TimeoutError):
            continue
        if msg.command in commands:
            return msg

    return None


async def _say(client, target, text):
    """Say something and hand back the identifier the network gave it."""
    await client.send(f"PRIVMSG {target} :{text}")
    echo = await _wait_for(client, "PRIVMSG")
    assert echo is not None, f"no echo for {text!r}"
    return _tag(echo, "msgid")


# --------------------------------------------------------------------- #
# SEARCH                                                                #
# --------------------------------------------------------------------- #


async def test_search_finds_one_message_in_a_channel(schema, ircd_store):
    """The word, and only the message that has it."""
    chan = f"#srch1{TAG}"
    speaker = await _client(ircd_store, f"ssp1{TAG}")
    await speaker.send(f"JOIN {chan}")
    await _drain(speaker)

    await _say(speaker, chan, "the kettle is on")
    wanted = await _say(speaker, chan, "the marmalade is in the cupboard")
    await _say(speaker, chan, "nothing to report")
    await asyncio.sleep(2)
    await _drain(speaker)

    opened, found, _ = await _chathistory(
        speaker, f"SEARCH {chan} :marmalade")

    assert opened.command == "BATCH", opened
    assert _bodies(found) == ["the marmalade is in the cupboard"]
    assert _tag(found[0], "msgid") == wanted

    await speaker.disconnect()


async def test_search_reaches_only_what_you_may_read(schema, ircd_store):
    """A channel you are not on is not searched, and is not an error.

    Being told "no results" and being told "you may not" are the same
    answer here on purpose: the second tells a stranger that the word is
    in a channel they cannot see.
    """
    chan = f"#srch2{TAG}"
    other = f"#srch3{TAG}"

    speaker = await _client(ircd_store, f"ssp2{TAG}")
    await speaker.send(f"JOIN {chan}")
    await _drain(speaker)
    await _say(speaker, chan, "the aardvark has escaped")

    stranger = await _client(ircd_store, f"ssp3{TAG}")
    await stranger.send(f"JOIN {other}")
    await _drain(stranger)
    await asyncio.sleep(2)

    _opened, found, _ = await _chathistory(stranger, "SEARCH * :aardvark")
    assert _bodies(found) == []

    # And naming it outright is refused in words.
    fail, _, _ = await _chathistory(stranger, f"SEARCH {chan} :aardvark")
    assert fail.command == "FAIL", fail
    assert fail.params[1] == "INVALID_TARGET", fail.params

    await speaker.disconnect()
    await stranger.disconnect()


async def test_search_everywhere_covers_channels_and_conversations(
        schema, ircd_store, store_services):
    """`*` is every place this client may look, which is both kinds."""
    chan = f"#srch4{TAG}"
    a = await _identified(ircd_store, store_services, f"ssa{TAG}")
    b = await _identified(ircd_store, store_services, f"ssb{TAG}")

    await a.send(f"JOIN {chan}")
    await _drain(a)

    await _say(a, chan, "the pangolin is in the channel")
    await _say(a, f"ssb{TAG}", "the pangolin is in the conversation")
    await asyncio.sleep(2)
    await _drain(a)

    _opened, found, _ = await _chathistory(a, "SEARCH * :pangolin")

    assert sorted(_bodies(found)) == [
        "the pangolin is in the channel",
        "the pangolin is in the conversation",
    ]

    # Each message comes back under the target it was sent to, because a
    # search over everything answers with messages from several places.
    assert sorted(m.params[0] for m in found) == sorted([chan, f"ssb{TAG}"])

    await a.disconnect()
    await b.disconnect()


async def test_search_can_be_narrowed(schema, ircd_store, store_services):
    """from= and limit= are filters on the same query."""
    chan = f"#srch5{TAG}"
    a = await _identified(ircd_store, store_services, f"ssc{TAG}")
    b = await _identified(ircd_store, store_services, f"ssd{TAG}")

    for client in (a, b):
        await client.send(f"JOIN {chan}")
        await _drain(client)

    await _say(a, chan, "wombat one")
    await _say(b, chan, "wombat two")
    await asyncio.sleep(2)
    await _drain(a)

    _opened, found, _ = await _chathistory(
        a, f"SEARCH {chan} from=ssd{TAG} :wombat")
    assert _bodies(found) == ["wombat two"]

    _opened, found, _ = await _chathistory(
        a, f"SEARCH {chan} limit=1 :wombat")
    assert len(found) == 1

    await a.disconnect()
    await b.disconnect()


async def test_a_search_with_no_text_is_refused(schema, ircd_store):
    """And so is an option the command does not know."""
    client = await _client(ircd_store, f"sse{TAG}")
    await client.send(f"JOIN #srch6{TAG}")
    await _drain(client)

    # Nothing to look for at all: the ordinary numeric, because that is
    # what a command with too few parameters answers with everywhere else.
    await client.send(f"SEARCH #srch6{TAG}")
    refused = await _wait_for(client, "461", "FAIL")
    assert refused is not None, "a search with no text was accepted"

    await _drain(client)

    fail, _, _ = await _chathistory(
        client, f"SEARCH #srch6{TAG} colour=blue :anything")
    assert fail.command == "FAIL", fail
    assert fail.params[1] == "INVALID_PARAMS", fail.params

    await client.disconnect()


async def test_a_search_that_is_not_an_expression_still_answers(
        schema, ircd_store):
    """What arrives is what somebody typed, brackets and apostrophes and all.

    to_tsquery() raises an error on any of that; websearch_to_tsquery()
    takes prose, which is what a box somebody types into needs.
    """
    chan = f"#srch7{TAG}"
    client = await _client(ircd_store, f"ssf{TAG}")
    await client.send(f"JOIN {chan}")
    await _drain(client)
    await _say(client, chan, "it is what it is")
    await asyncio.sleep(2)
    await _drain(client)

    _opened, found, _ = await _chathistory(
        client, f"SEARCH {chan} :it's ((a mess")
    assert _bodies(found) == []

    await client.disconnect()


# --------------------------------------------------------------------- #
# EDIT                                                                  #
# --------------------------------------------------------------------- #


async def test_an_edit_changes_the_message_and_keeps_its_name(
        schema, ircd_store, store_services):
    """The channel is told, and the store now says the new thing."""
    chan = f"#edit1{TAG}"
    author = await _identified(ircd_store, store_services, f"eda{TAG}")
    watcher = await _identified(ircd_store, store_services, f"edb{TAG}")

    for client in (author, watcher):
        await client.send(f"JOIN {chan}")
        await _drain(client)

    msgid = await _say(author, chan, "meet at sevn")
    await asyncio.sleep(1.5)
    await _drain(watcher)

    await author.send(f"EDIT {chan} {msgid} :meet at seven")

    told = await _wait_for(watcher, "EDIT")
    assert told is not None, "the channel was never told"
    assert told.params[0] == chan
    assert told.params[1] == msgid
    assert told.params[2] == "meet at seven"

    await asyncio.sleep(1)
    await _drain(author)

    _opened, rows, _ = await _chathistory(
        author, f"CHATHISTORY LATEST {chan} * 10")

    assert _bodies(rows) == ["meet at seven"]
    # The identifier does not change: the replies and the reactions point
    # at it, and this is still the message they are about.
    assert _tag(rows[0], "msgid") == msgid
    # And the replay says it is not the text that was sent.
    assert _tag(rows[0], "blacknode/edited"), rows[0].tags

    await author.disconnect()
    await watcher.disconnect()


async def test_only_the_author_may_edit(schema, ircd_store, store_services):
    """Not a channel operator, who may redact instead."""
    chan = f"#edit2{TAG}"
    author = await _identified(ircd_store, store_services, f"edc{TAG}")
    op = await _identified(ircd_store, store_services, f"edd{TAG}")

    # The operator joins first, so the channel is theirs.
    await op.send(f"JOIN {chan}")
    await _drain(op)
    await author.send(f"JOIN {chan}")
    await _drain(author)

    msgid = await _say(author, chan, "something worth keeping")
    await asyncio.sleep(1.5)
    await _drain(op)

    await op.send(f"EDIT {chan} {msgid} :something else entirely")
    fail = await _wait_for(op, "FAIL")

    assert fail is not None, "the edit was not refused"
    assert fail.params[1] == "EDIT_FORBIDDEN", fail.params

    await author.disconnect()
    await op.disconnect()


async def test_an_empty_edit_is_not_a_deletion(schema, ircd_store, store_services):
    """There is a command for that, and this is not it."""
    chan = f"#edit3{TAG}"
    author = await _identified(ircd_store, store_services, f"ede{TAG}")
    await author.send(f"JOIN {chan}")
    await _drain(author)

    msgid = await _say(author, chan, "still here")
    await asyncio.sleep(1.5)
    await _drain(author)

    await author.send(f"EDIT {chan} {msgid} :")
    refused = await _wait_for(author, "FAIL", "461")

    assert refused is not None, "an empty edit was accepted"

    await author.disconnect()


async def test_editing_a_message_that_is_not_there(schema, ircd_store, store_services):
    """A name nobody has heard of, and the right message named in the
    wrong place."""
    chan = f"#edit4{TAG}"
    other = f"#edit5{TAG}"
    author = await _identified(ircd_store, store_services, f"edf{TAG}")

    for name in (chan, other):
        await author.send(f"JOIN {name}")
    await _drain(author)

    msgid = await _say(author, chan, "in the first channel")
    await asyncio.sleep(1.5)
    await _drain(author)

    await author.send(f"EDIT {chan} not-a-real-msgid :nonsense")
    fail = await _wait_for(author, "FAIL")
    assert fail is not None
    assert fail.params[1] == "UNKNOWN_MSGID", fail.params

    await _drain(author)

    await author.send(f"EDIT {other} {msgid} :moved house")
    fail = await _wait_for(author, "FAIL")
    assert fail is not None
    assert fail.params[1] == "UNKNOWN_MSGID", fail.params

    await author.disconnect()
