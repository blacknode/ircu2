"""IRCv3 batches and labeled responses.

A client names its request and gets the answer back under the same name, so
it can tell which reply belongs to which command -- something a terminal
client never needed and a graphical one cannot work without.  And a client
that asked for none of it gets exactly the lines it always got, which is
the half these tests spend most of their effort on.
"""

import re

import pytest

from irc_client import IRCClient


pytestmark = pytest.mark.single_server

LABEL = re.compile(r"(?:^|;)label=([^;\s]+)")
BATCH = re.compile(r"(?:^|;)batch=([^;\s]+)")


def label_of(msg):
    m = LABEL.search(msg.tags or "")
    return m.group(1) if m else None


def batch_of(msg):
    m = BATCH.search(msg.tags or "")
    return m.group(1) if m else None


async def collect(client, count, timeout=5.0):
    """Read `count` messages, whatever they are."""
    out = []
    for _ in range(count):
        out.append(await client._recv_from_stream(timeout=timeout))
    return out


@pytest.fixture
async def labeled_client(ircd_hub):
    cli = IRCClient()
    await cli.connect(ircd_hub["host"], ircd_hub["port"])
    await cli.negotiate_cap(["message-tags", "batch", "labeled-response"])
    await cli.register("labuser", "testuser", "Labeled")
    # Everything these tests assert on is "the next line after the
    # command", so whatever the server is still saying about the
    # connection has to be out of the way first.
    await cli.drain()
    yield cli
    await cli.disconnect()


async def test_capabilities_are_advertised(ircd_hub):
    """batch and labeled-response show up in CAP LS and can be requested."""
    cli = IRCClient()
    await cli.connect(ircd_hub["host"], ircd_hub["port"])
    try:
        acked = await cli.negotiate_cap(["batch", "labeled-response"])
        assert "batch" in acked
        assert "labeled-response" in acked
    finally:
        await cli.disconnect()


async def test_several_replies_are_wrapped_in_a_batch(labeled_client):
    """A command that answers more than once answers inside one batch."""
    await labeled_client.send_raw(b"@label=abc123 LUSERS")

    opened = await labeled_client._recv_from_stream(timeout=5.0)
    assert opened.command == "BATCH"
    assert opened.params[0].startswith("+")
    assert opened.params[1] == "labeled-response"
    assert label_of(opened) == "abc123"

    batch_id = opened.params[0][1:]

    # Everything until the close belongs to the batch.
    closed = None
    inside = 0
    for _ in range(20):
        msg = await labeled_client._recv_from_stream(timeout=5.0)
        if msg.command == "BATCH" and msg.params[0] == f"-{batch_id}":
            closed = msg
            break
        assert batch_of(msg) == batch_id, f"loose message in a batch: {msg.raw!r}"
        inside += 1

    assert closed is not None, "the batch was never closed"
    assert inside > 1, "this command was supposed to answer more than once"

    # The label names the batch; it is not repeated on the closing line.
    assert label_of(closed) is None


async def test_a_command_that_answers_nothing_gets_an_ack(labeled_client):
    """PONG produces no reply, so the label comes back as a bare ACK.

    Without this a client would be left waiting for an answer that was
    never going to come, which is the thing the label exists to prevent.
    """
    await labeled_client.send_raw(b"@label=quiet PONG :nothing")

    msg = await labeled_client._recv_from_stream(timeout=5.0)
    assert msg.command == "ACK"
    assert label_of(msg) == "quiet"


async def test_an_error_is_the_answer_too(labeled_client):
    """A command that fails is still answered under its label."""
    await labeled_client.send_raw(b"@label=err WHOIS nosuchnickhere")

    opened = await labeled_client._recv_from_stream(timeout=5.0)
    assert opened.command == "BATCH"
    assert label_of(opened) == "err"

    batch_id = opened.params[0][1:]
    numerics = []
    for _ in range(10):
        msg = await labeled_client._recv_from_stream(timeout=5.0)
        if msg.command == "BATCH":
            break
        assert batch_of(msg) == batch_id
        numerics.append(msg.command)

    assert "401" in numerics, f"expected ERR_NOSUCHNICK, got {numerics}"


async def test_an_unlabeled_command_is_untouched(labeled_client):
    """The capability changes nothing until a label is actually sent."""
    await labeled_client.send_raw(b"LUSERS")

    msg = await labeled_client._recv_from_stream(timeout=5.0)
    assert msg.command != "BATCH"
    assert batch_of(msg) is None
    assert label_of(msg) is None


async def test_labeled_response_without_batch_is_ignored(ircd_hub):
    """The specification builds one on the other.

    A client that asked for the label but not the batch cannot be answered
    the way the specification says, so it is answered the way it would have
    been before it asked for anything -- rather than half way, which no
    client knows how to read.
    """
    cli = IRCClient()
    await cli.connect(ircd_hub["host"], ircd_hub["port"])
    await cli.negotiate_cap(["message-tags", "labeled-response"])
    await cli.register("nobatch", "testuser", "No Batch")

    try:
        await cli.send_raw(b"@label=abc LUSERS")
        msg = await cli._recv_from_stream(timeout=5.0)
        assert msg.command != "BATCH"
        assert label_of(msg) is None
        assert batch_of(msg) is None
    finally:
        await cli.disconnect()


async def test_a_traditional_client_sees_nothing(ircd_hub):
    """A client that negotiated nothing gets the lines it always got.

    Including when it sends a label of its own: the tag is dropped with
    every other server tag a client is not allowed to set, and the answer
    comes back plain.
    """
    cli = IRCClient()
    await cli.connect(ircd_hub["host"], ircd_hub["port"])
    await cli.register("plainlab", "testuser", "Plain")

    try:
        await cli.send_raw(b"@label=abc LUSERS")
        for _ in range(3):
            msg = await cli._recv_from_stream(timeout=5.0)
            assert msg.command != "BATCH"
            assert msg.tags == "", f"a traditional client was sent tags: {msg.raw!r}"
            assert not msg.raw.startswith("@")
    finally:
        await cli.disconnect()


async def test_two_labeled_commands_do_not_share_a_batch(labeled_client):
    """Each command gets its own batch, and its own identifier."""
    seen = []

    for label in ("one", "two"):
        await labeled_client.send_raw(f"@label={label} LUSERS".encode())
        opened = await labeled_client._recv_from_stream(timeout=5.0)
        assert opened.command == "BATCH"
        assert label_of(opened) == label
        batch_id = opened.params[0][1:]
        seen.append(batch_id)

        for _ in range(20):
            msg = await labeled_client._recv_from_stream(timeout=5.0)
            if msg.command == "BATCH" and msg.params[0] == f"-{batch_id}":
                break

    assert seen[0] != seen[1], f"two commands shared a batch: {seen}"
