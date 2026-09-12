"""Regression: server<->services RPC must not carry a client @time tag on S2S.

XQUERY/XREPLY (P10 XQ/XR) are internal server-to-services RPC (the
spamfilter's hold/release exchange; iauth extension queries).  They are
consumed by services software that parses P10 fields positionally and does
not strip IRCv3 message-tags.  The S2S time-tagging introduced with
message-tags used a blacklist that missed XQ/XR, so the hub began
prepending `@time=...` to XQUERY -- shifting every field by one and
breaking the routing token ('spam:<token>' landed one slot over).

A client-facing relayed event (PRIVMSG/NOTICE/etc.) legitimately keeps its
@time on S2S; only the service-RPC commands must stay untagged.

The XQUERY is provoked through the spamfilter: an S-line that holds a
private message makes the hub ask the spamfilter server what to do with it.
(SASL, the other historical trigger, no longer exists.)
"""

import asyncio
import time

import pytest

from irc_client import IRCClient
from p10_server import P10Server

pytestmark = pytest.mark.single_server

SERVICES = "services.test.net"


@pytest.fixture
async def services(ircd_hub):
    """Fake P10 services server linked to the hub, acting as spamfilter."""
    srv = P10Server(name=SERVICES, numeric=4, password="testpass")
    await srv.connect(ircd_hub["host"], ircd_hub["server_port"])
    await srv.handshake()
    await srv.send_config("sline.server", SERVICES)
    await asyncio.sleep(0.3)
    yield srv
    await srv.disconnect()


async def test_xquery_has_no_time_tag(ircd_hub, services):
    """The XQUERY the hub sends to services must be plain P10 (no @tag prefix).

    Asserts on the raw wire line so a leading `@time=` is caught directly,
    independent of any tag-stripping the harness does for token matching.
    """
    sender = IRCClient()
    await sender.connect(ircd_hub["host"], ircd_hub["port"])
    await sender.register("rpcuntag1", "testuser", "RPC Untagged")

    receiver = IRCClient()
    await receiver.connect(ircd_hub["host"], ircd_hub["port"])
    await receiver.register("rpcuntag2", "testuser", "RPC Untagged")

    try:
        await services.send_sline("rpcspamword", msg_type="P",
                                  expire=int(time.time()) + 30)
        await asyncio.sleep(0.5)
        await services.wait_for_user("rpcuntag1")
        await services.wait_for_user("rpcuntag2")
        await services.drain_messages()

        await sender.send("PRIVMSG rpcuntag2 :this rpcspamword is held")

        line = await services.wait_for_token("XQ", timeout=5.0)

        # Raw line must be classic P10: first field is the hub numeric, not a
        # message-tag section.
        assert not line.lstrip().startswith("@"), f"XQUERY carried a tag: {line!r}"
        parts = line.split()
        assert "time=" not in parts[0], f"XQUERY carried a time tag: {line!r}"
        # Classic P10 layout: <hubnum> XQ <svcnum> <routing> :<payload>.
        # A leading @time= would shift all of these one slot to the right.
        assert parts[1] == "XQ", f"unexpected XQUERY layout: {line!r}"
        assert parts[3].startswith("spam:"), f"routing shifted by a tag: {line!r}"

        # Let the held message go so nothing lingers in the hold queue.
        await services.send_xreply(parts[0], parts[3], "YES")
    finally:
        for c in (sender, receiver):
            try:
                await c.send("QUIT :cleanup")
            except Exception:
                pass
            await c.disconnect()
