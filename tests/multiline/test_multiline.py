"""IRCv3 draft/multiline: a message longer than a line.

The client sends the pieces as a batch; the server puts them back together
and relays them.  A client that asked for multiline sees them wrapped in a
batch of its own; every other client sees the separate messages it has
always seen, which is the half most of these tests are about.
"""

import re

import pytest

from irc_client import IRCClient


pytestmark = pytest.mark.single_server

MSGID = re.compile(r"(?:^|;)msgid=([^;\s]+)")
BATCH = re.compile(r"(?:^|;)batch=([^;\s]+)")

MAX_LINES = 24

CONCAT = re.compile(r"(?:^|;)draft/multiline-concat(?:=|;|$)")


def has_concat(msg):
    return bool(CONCAT.search(msg.tags or ""))


def msgid_of(msg):
    m = MSGID.search(msg.tags or "")
    return m.group(1) if m else None


def batch_of(msg):
    m = BATCH.search(msg.tags or "")
    return m.group(1) if m else None


@pytest.fixture
async def party(ircd_hub):
    """A sender and two watchers: one with multiline, one with nothing."""
    sender, watcher, plain = IRCClient(), IRCClient(), IRCClient()

    for cli, nick, caps in (
        (sender, "mlsend", ["message-tags", "batch", "draft/multiline"]),
        (watcher, "mlwatch", ["message-tags", "batch", "draft/multiline"]),
        (plain, "mlplain", None),
    ):
        await cli.connect(ircd_hub["host"], ircd_hub["port"])
        if caps:
            await cli.negotiate_cap(caps)
        await cli.register(nick, "testuser", nick)

    for cli in (sender, watcher, plain):
        await cli.send_raw(b"JOIN #ml")
        await cli.wait_for("JOIN", timeout=5.0)

    yield sender, watcher, plain

    for cli in (sender, watcher, plain):
        await cli.disconnect()


async def test_capability_advertises_its_limits(ircd_hub):
    """A client has to know what it may send before it sends it."""
    cli = IRCClient()
    await cli.connect(ircd_hub["host"], ircd_hub["port"])
    try:
        await cli.send_raw(b"CAP LS 302")
        msg = await cli.wait_for("CAP", timeout=5.0)
        caps = msg.params[-1]
        entry = [c for c in caps.split() if c.startswith("draft/multiline")]
        assert entry, f"draft/multiline not advertised: {caps}"
        assert "max-bytes=" in entry[0]
        assert "max-lines=" in entry[0]
    finally:
        await cli.disconnect()


async def test_a_long_message_arrives_as_one_batch(party):
    """The pieces come back wrapped, with one identifier for the whole."""
    sender, watcher, _plain = party

    await sender.send_raw(b"BATCH +q draft/multiline #ml")
    await sender.send_raw(b"@batch=q PRIVMSG #ml :first")
    await sender.send_raw(b"@batch=q PRIVMSG #ml :second")
    await sender.send_raw(b"BATCH -q")

    opened = await watcher.wait_for("BATCH", timeout=5.0)
    assert opened.params[0].startswith("+")
    assert opened.params[1] == "draft/multiline"
    assert opened.params[2] == "#ml"

    # One name for the whole message, on the line that opens it.  The
    # pieces are pieces, not messages of their own.
    whole = msgid_of(opened)
    assert whole, f"the message has no identifier: {opened.raw!r}"

    batch_id = opened.params[0][1:]
    texts = []
    for _ in range(6):
        msg = await watcher._recv_from_stream(timeout=5.0)
        if msg.command == "BATCH":
            assert msg.params[0] == f"-{batch_id}"
            assert msgid_of(msg) is None, "the closing line is a delimiter"
            break
        assert msg.command == "PRIVMSG"
        assert batch_of(msg) == batch_id
        assert msgid_of(msg) is None, "a piece was given a name of its own"
        texts.append(msg.params[-1])

    assert texts == ["first", "second"]


async def test_a_traditional_client_sees_separate_messages(party):
    """No batch, no tags: the line a traditional client always got."""
    sender, _watcher, plain = party

    await sender.send_raw(b"BATCH +q draft/multiline #ml")
    await sender.send_raw(b"@batch=q PRIVMSG #ml :first")
    await sender.send_raw(b"@batch=q PRIVMSG #ml :second")
    await sender.send_raw(b"BATCH -q")

    for expected in ("first", "second"):
        msg = await plain.wait_for_user_msg("PRIVMSG", timeout=5.0)
        assert msg.params[-1] == expected
        assert msg.tags == "", f"a traditional client was sent tags: {msg.raw!r}"
        assert not msg.raw.startswith("@")


async def test_concat_joins_without_a_line_break(party):
    """draft/multiline-concat continues the piece before it.

    This is what lets a client send a line longer than a line: the pieces
    are how it travels, not how it reads.
    """
    sender, watcher, _plain = party

    await sender.send_raw(b"BATCH +q draft/multiline #ml")
    await sender.send_raw(b"@batch=q PRIVMSG #ml :one")
    await sender.send_raw(
        b"@batch=q;draft/multiline-concat PRIVMSG #ml : and more")
    await sender.send_raw(b"BATCH -q")

    await watcher.wait_for("BATCH", timeout=5.0)
    msg = await watcher.wait_for("PRIVMSG", timeout=5.0)
    assert msg.params[-1] == "one and more"


async def test_a_line_longer_than_a_line_survives_whole(party):
    """The pieces of one line are joined, and the line is re-split to fit.

    A line assembled out of concat pieces can be longer than anything the
    wire can carry.  Relaying it as one PRIVMSG does not make the line
    longer -- it makes the send layer drop the tail, silently -- so it
    goes out as however many lines it takes, every one after the first
    marked draft/multiline-concat.  What the client joins back has to be
    what was sent, byte for byte.
    """
    sender, watcher, _plain = party
    whole = "".join(f"[{i:03d}]abcdefghij" for i in range(100))  # 1500 bytes

    await sender.send_raw(b"BATCH +q draft/multiline #ml")
    for i in range(0, len(whole), 300):
        tag = b"@batch=q" if i == 0 else b"@batch=q;draft/multiline-concat"
        await sender.send_raw(tag + f" PRIVMSG #ml :{whole[i:i + 300]}".encode())
    await sender.send_raw(b"BATCH -q")

    # Generous: the bytes are charged even though the flat per-command
    # penalty is not, so a message this size is deliberately slowed down.
    opened = await watcher.wait_for("BATCH", timeout=20.0)
    batch_id = opened.params[0][1:]

    joined = ""
    pieces = 0
    for _ in range(16):
        msg = await watcher._recv_from_stream(timeout=10.0)
        if msg.command == "BATCH":
            assert msg.params[0] == f"-{batch_id}"
            break
        assert msg.command == "PRIVMSG"
        assert batch_of(msg) == batch_id
        # The first piece starts the line; every one after it continues
        # it, or the client would show line breaks nobody typed.
        assert has_concat(msg) == (pieces > 0), msg.raw
        assert len(msg.raw) < 1024, f"a line was sent too long: {len(msg.raw)}"
        joined += msg.params[-1]
        pieces += 1

    assert pieces > 1, "the line fitted after all; make the test message longer"
    assert joined == whole


async def test_a_traditional_client_is_never_told_about_concat(party):
    """It gets the pieces, whole, and not one tag.

    The re-split is visible to it as the series of separate messages it
    has always seen.  A tag saying "this continues the last one" belongs
    only to a client that asked for draft/multiline.
    """
    sender, _watcher, plain = party
    whole = "".join(f"[{i:03d}]abcdefghij" for i in range(100))

    await sender.send_raw(b"BATCH +q draft/multiline #ml")
    for i in range(0, len(whole), 300):
        tag = b"@batch=q" if i == 0 else b"@batch=q;draft/multiline-concat"
        await sender.send_raw(tag + f" PRIVMSG #ml :{whole[i:i + 300]}".encode())
    await sender.send_raw(b"BATCH -q")

    joined = ""
    while len(joined) < len(whole):
        msg = await plain.wait_for_user_msg("PRIVMSG", timeout=20.0)
        assert msg.tags == "", f"a traditional client was sent tags: {msg.raw!r}"
        joined += msg.params[-1]

    assert joined == whole


async def test_the_whole_thing_at_full_speed(party):
    """The pieces are one message, and are charged as one.

    A client sending the lines the specification allows, as fast as it can,
    must not be throttled off the server for sending one message.  Before
    the parser learned to charge a batch once, this test disconnected the
    sender.
    """
    sender, watcher, _plain = party

    await sender.send_raw(b"BATCH +q draft/multiline #ml")
    for i in range(MAX_LINES):
        await sender.send_raw(f"@batch=q PRIVMSG #ml :line {i}".encode())
    await sender.send_raw(b"BATCH -q")

    opened = await watcher.wait_for("BATCH", timeout=10.0)
    batch_id = opened.params[0][1:]

    seen = 0
    for _ in range(MAX_LINES + 2):
        msg = await watcher._recv_from_stream(timeout=10.0)
        if msg.command == "BATCH":
            break
        assert batch_of(msg) == batch_id
        seen += 1

    assert seen == MAX_LINES


async def test_too_many_lines_is_refused(party):
    """Past max-lines the pieces are refused, and the client is told."""
    sender, watcher, _plain = party

    await sender.send_raw(b"BATCH +z draft/multiline #ml")
    for i in range(MAX_LINES + 6):
        await sender.send_raw(f"@batch=z PRIVMSG #ml :x{i}".encode())

    msg = await sender.wait_for("417", timeout=10.0)
    assert msg is not None

    await sender.send_raw(b"BATCH -z")
    opened = await watcher.wait_for("BATCH", timeout=10.0)
    batch_id = opened.params[0][1:]

    seen = 0
    for _ in range(MAX_LINES + 4):
        msg = await watcher._recv_from_stream(timeout=10.0)
        if msg.command == "BATCH":
            break
        assert batch_of(msg) == batch_id
        seen += 1

    assert seen == MAX_LINES, f"the cap did not hold: {seen} lines"


async def test_a_batch_without_the_capability_is_refused(ircd_hub):
    """A client that did not ask to send batches cannot send one."""
    cli = IRCClient()
    await cli.connect(ircd_hub["host"], ircd_hub["port"])
    await cli.register("mlnocap", "testuser", "No Cap")

    try:
        await cli.send_raw(b"BATCH +q draft/multiline #ml")
        msg = await cli.wait_for("421", timeout=5.0)
        assert "BATCH" in msg.raw
    finally:
        await cli.disconnect()
