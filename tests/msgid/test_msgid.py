"""IRCv3 message identifiers (@msgid).

One message has one name, network-wide, and the clients that did not ask for
tags never see it.  That second half is the point of most of these: the
identifier is an addition for the clients that negotiated message-tags, never
a change to what a traditional client is sent.
"""

import re

import pytest

from irc_client import IRCClient

from pr_msgtags_compat.helpers import join_synced
from p10_server import P10Server


pytestmark = pytest.mark.multi_server

MSGID = re.compile(r"(?:^|;)msgid=([^;\s]+)")


def msgid_of(msg):
    """The identifier on a message, or None."""
    m = MSGID.search(msg.tags or "")
    return m.group(1) if m else None


@pytest.fixture
async def services(ircd_network):
    hub = ircd_network["hub"]
    srv = P10Server(name="services.test.net", numeric=4, password="testpass")
    await srv.connect(hub["host"], hub["server_port"])
    await srv.handshake()
    yield srv
    await srv.disconnect()


async def test_traditional_client_sees_no_tags(ircd_network):
    """A client that negotiated nothing gets the line it always got.

    This is the compatibility rule, checked on the one command that now has
    something extra to say: if a msgid ever leaks to a client that did not ask
    for tags, this is what catches it.
    """
    hub = ircd_network["hub"]

    old = IRCClient()
    await old.connect(hub["host"], hub["port"])
    await old.register("msgidold", "testuser", "No Caps Here")

    sender = IRCClient()
    await sender.connect(hub["host"], hub["port"])
    await sender.register("msgidsnd", "testuser", "Sender")

    try:
        await join_synced("#msgid1", old, sender)
        await sender.send_raw(b"PRIVMSG #msgid1 :plain please")

        msg = await old.wait_for_user_msg("PRIVMSG")
        assert msg.params[-1] == "plain please"
        assert msg.tags == "", f"a traditional client was sent tags: {msg.raw!r}"
        assert not msg.raw.startswith("@")
    finally:
        await old.disconnect()
        await sender.disconnect()


async def test_message_tags_client_gets_a_msgid(ircd_network):
    """A client with message-tags gets an identifier on PRIVMSG and NOTICE."""
    hub = ircd_network["hub"]

    obs = IRCClient()
    await obs.connect(hub["host"], hub["port"])
    await obs.negotiate_cap(["message-tags"])
    await obs.register("msgidobs", "testuser", "Observer")

    sender = IRCClient()
    await sender.connect(hub["host"], hub["port"])
    await sender.register("msgidsn2", "testuser", "Sender")

    try:
        await join_synced("#msgid2", obs, sender)

        await sender.send_raw(b"PRIVMSG #msgid2 :uno")
        first = await obs.wait_for_user_msg("PRIVMSG")
        assert msgid_of(first), f"no msgid on {first.raw!r}"

        await sender.send_raw(b"PRIVMSG #msgid2 :dos")
        second = await obs.wait_for_user_msg("PRIVMSG")
        assert msgid_of(second) != msgid_of(first), "two messages, one name"

        await sender.send_raw(b"NOTICE #msgid2 :aviso")
        notice = await obs.wait_for_user_msg("NOTICE")
        assert msgid_of(notice), f"no msgid on {notice.raw!r}"
    finally:
        await obs.disconnect()
        await sender.disconnect()


async def test_echo_carries_the_same_msgid(ircd_network):
    """The sender's echo and everyone else's copy are the same message."""
    hub = ircd_network["hub"]

    obs = IRCClient()
    await obs.connect(hub["host"], hub["port"])
    await obs.negotiate_cap(["message-tags"])
    await obs.register("msgidob3", "testuser", "Observer")

    sender = IRCClient()
    await sender.connect(hub["host"], hub["port"])
    await sender.negotiate_cap(["message-tags", "echo-message"])
    await sender.register("msgidsn3", "testuser", "Sender")

    try:
        await join_synced("#msgid3", obs, sender)
        await sender.send_raw(b"PRIVMSG #msgid3 :juntos")

        seen = await obs.wait_for_user_msg("PRIVMSG")
        echoed = await sender.wait_for_user_msg("PRIVMSG")

        assert msgid_of(seen), f"no msgid on {seen.raw!r}"
        assert msgid_of(seen) == msgid_of(echoed), (
            f"the sender and the channel disagree: "
            f"{msgid_of(echoed)} vs {msgid_of(seen)}"
        )
    finally:
        await obs.disconnect()
        await sender.disconnect()


async def test_numeric_reply_has_no_msgid(ircd_network):
    """A numeric sent while handling a message is not that message.

    Without this the 403 for a PRIVMSG to a channel that does not exist
    inherits the message's name, and anything storing messages files the
    error under it.
    """
    hub = ircd_network["hub"]

    cli = IRCClient()
    await cli.connect(hub["host"], hub["port"])
    await cli.negotiate_cap(["message-tags"])
    await cli.register("msgidnum", "testuser", "Numerics")

    try:
        await cli.send_raw(b"PRIVMSG #msgid-nope-nothing-here :x")
        msg = await cli.wait_for("403")
        assert msgid_of(msg) is None, f"a numeric carried a msgid: {msg.raw!r}"
    finally:
        await cli.disconnect()


async def test_client_cannot_choose_its_own_msgid(ircd_network):
    """A client's @msgid= is discarded and replaced.

    An identifier a client could choose is one it could use to point at, or
    overwrite, somebody else's message in whatever stores them.
    """
    hub = ircd_network["hub"]

    obs = IRCClient()
    await obs.connect(hub["host"], hub["port"])
    await obs.negotiate_cap(["message-tags"])
    await obs.register("msgidob4", "testuser", "Observer")

    sender = IRCClient()
    await sender.connect(hub["host"], hub["port"])
    await sender.negotiate_cap(["message-tags"])
    await sender.register("msgidsn4", "testuser", "Sender")

    try:
        await join_synced("#msgid4", obs, sender)
        await sender.send_raw(b"@msgid=FORGED PRIVMSG #msgid4 :spoof")

        msg = await obs.wait_for_user_msg("PRIVMSG")
        got = msgid_of(msg)
        assert got is not None
        assert got != "FORGED", f"a client chose its own identifier: {msg.raw!r}"
    finally:
        await obs.disconnect()
        await sender.disconnect()


async def test_msgid_crosses_the_link(ircd_network, services):
    """The identifier a local message goes out with is the one clients saw.

    The whole point: two clients on opposite ends of the network have to agree
    on what to call the same message.
    """
    hub = ircd_network["hub"]

    obs = IRCClient()
    await obs.connect(hub["host"], hub["port"])
    await obs.negotiate_cap(["message-tags"])
    await obs.register("msgidob5", "testuser", "Observer")

    sender = IRCClient()
    await sender.connect(hub["host"], hub["port"])
    await sender.register("msgidsn5", "testuser", "Sender")

    try:
        remote = await services.introduce_user("msgidrem")
        await join_synced("#msgid5", obs, sender)
        await services.send_join(remote, "#msgid5")

        await sender.send_raw(b"PRIVMSG #msgid5 :cruzando")

        seen = await obs.wait_for_user_msg("PRIVMSG")
        local_id = msgid_of(seen)
        assert local_id, f"no msgid on {seen.raw!r}"

        lines = await services.recv_until("P", timeout=5.0)
        relayed = [line for line in lines if "cruzando" in line]
        assert relayed, f"the message never reached the peer: {lines!r}"
        assert f"msgid={local_id}" in relayed[0], (
            f"the link calls it something else: {relayed[0]!r}"
        )
    finally:
        await obs.disconnect()
        await sender.disconnect()


async def test_msgid_from_a_server_is_kept(ircd_network, services):
    """An identifier that arrives from a peer is the network's, and is kept."""
    hub = ircd_network["hub"]

    obs = IRCClient()
    await obs.connect(hub["host"], hub["port"])
    await obs.negotiate_cap(["message-tags"])
    await obs.register("msgidob6", "testuser", "Observer")

    old = IRCClient()
    await old.connect(hub["host"], hub["port"])
    await old.register("msgidol6", "testuser", "No Caps")

    try:
        numnick = await services.wait_for_user("msgidob6")
        await services._send(f"@msgid=XY123abc {services._num} P {numnick} :de fuera")

        # The message is from a *server*, so the user-message helper --
        # which exists to skip the server's own notices -- would skip this
        # one too.  Matching on the text is what tells them apart.
        msg = await obs.wait_for_message_with_text("PRIVMSG", "de fuera")
        assert msg.params[-1] == "de fuera"
        assert msgid_of(msg) == "XY123abc", (
            f"upstream's identifier was not kept: {msg.raw!r}"
        )
    finally:
        await obs.disconnect()
        await old.disconnect()
