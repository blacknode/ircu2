"""TRUST_USERNAME display: the visible identity of a hidden user.

Every user is hidden from registration on (doc/readme.accounting).  With
TRUST_USERNAME on, what other users see is ``user@<vhost>`` -- the tilde
dropped and the host the cipher of the address -- while the server itself
keeps, propagates and shows operators the real ``~user@realhost``.
"""

import asyncio

import pytest

from irc_client import IRCClient
from p10_server import P10Server

from trust_username.helpers import (
    REAL_HOST,
    VIS_HOST,
    oper_up,
    whois_actual,
    whois_userline,
)


pytestmark = pytest.mark.multi_server


@pytest.fixture
async def services(ircd_network):
    hub = ircd_network["hub"]
    srv = P10Server(name="services.test.net", numeric=4, password="testpass")
    await srv.connect(hub["host"], hub["server_port"])
    await srv.handshake()
    yield srv
    await srv.disconnect()


async def test_whois_shows_untilded_username_and_hidden_host(ircd_network):
    """From the first WHOIS on, others see ``testuser@<vhost>``."""
    hub = ircd_network["hub"]

    user = IRCClient()
    await user.connect(hub["host"], hub["port"])
    await user.register("tu71w", "testuser", "Test User")

    observer = IRCClient()
    await observer.connect(hub["host"], hub["port"])
    await observer.register("tu71w2", "testuser", "Test User")

    try:
        username, host = await whois_userline(observer, "tu71w")
        assert username == "testuser", f"WHOIS should drop the tilde, got {username!r}"
        assert host == VIS_HOST, f"WHOIS should show the hidden host, got {host!r}"
    finally:
        for client in (user, observer):
            try:
                await client.send("QUIT :cleanup")
            except Exception:
                pass
            await client.disconnect()


async def test_whois_actual_shows_real_identity_to_oper(ircd_network):
    """An operator is also shown ``~user@realhost`` (338); a user is not."""
    hub = ircd_network["hub"]

    user = IRCClient()
    await user.connect(hub["host"], hub["port"])
    await user.register("tu71a", "testuser", "Test User")

    oper = IRCClient()
    await oper.connect(hub["host"], hub["port"])
    await oper.register("tu71ao", "testuser", "Test User")
    await oper_up(oper)

    plain = IRCClient()
    await plain.connect(hub["host"], hub["port"])
    await plain.register("tu71ap", "testuser", "Test User")

    try:
        real_user, real_host = await whois_actual(oper, "tu71a")
        assert real_user == "~testuser", real_user
        assert real_host == REAL_HOST, real_host

        await plain.send("WHOIS tu71a")
        msgs = await plain.collect_until("318", timeout=5.0)
        assert not [m for m in msgs if m.command == "338"], (
            f"a plain user must not be shown the real host: {msgs}"
        )
    finally:
        for client in (user, oper, plain):
            try:
                await client.send("QUIT :cleanup")
            except Exception:
                pass
            await client.disconnect()


async def test_s2s_nick_keeps_tilded_username_and_real_host(ircd_network, services):
    """Server propagation carries the real ``~user`` and real host: each
    server derives the hidden host itself from the address in the NICK."""
    hub = ircd_network["hub"]

    user = IRCClient()
    await user.connect(hub["host"], hub["port"])
    await user.register("tu71s", "testuser", "Test User")

    try:
        await services.wait_for_user("tu71s")
        recorded = services.users["tu71s"]
        assert recorded["username"].startswith("~"), (
            f"S2S NICK should keep tilded username, got {recorded['username']!r}"
        )
        assert recorded["host"] == REAL_HOST, (
            f"S2S NICK should carry the real host, got {recorded['host']!r}"
        )
        assert "x" in recorded["modes"], (
            f"S2S NICK should announce +x, got {recorded['modes']!r}"
        )
    finally:
        try:
            await user.send("QUIT :cleanup")
        except Exception:
            pass
        await user.disconnect()


async def test_userhost_shows_untilded_username(ircd_network):
    hub = ircd_network["hub"]

    user = IRCClient()
    await user.connect(hub["host"], hub["port"])
    await user.register("tu71uh", "testuser", "Test User")

    observer = IRCClient()
    await observer.connect(hub["host"], hub["port"])
    await observer.register("tu71uh2", "testuser", "Test User")

    try:
        await observer.send("USERHOST tu71uh")
        msg = await observer.wait_for("302", timeout=5.0)
        entry = msg.params[1]
        assert "=+testuser@" in entry, f"USERHOST should be untilded: {entry!r}"
        assert f"@{VIS_HOST}" in entry, entry
        assert "=+~testuser@" not in entry
    finally:
        for client in (user, observer):
            try:
                await client.send("QUIT :cleanup")
            except Exception:
                pass
            await client.disconnect()


async def test_uhnames_shows_untilded_username(ircd_network):
    hub = ircd_network["hub"]
    channel = "#tu71_names"

    user = IRCClient()
    await user.connect(hub["host"], hub["port"])
    acked = await user.negotiate_cap(["userhost-in-names"])
    assert "userhost-in-names" in acked
    await user.register("tu71n", "testuser", "Test User")

    observer = IRCClient()
    await observer.connect(hub["host"], hub["port"])
    acked = await observer.negotiate_cap(["userhost-in-names"])
    assert "userhost-in-names" in acked
    await observer.register("tu71n2", "testuser", "Test User")

    try:
        await user.send(f"JOIN {channel}")
        await user.wait_for("366")
        await observer.send(f"JOIN {channel}")
        await observer.wait_for("366")

        await observer.send(f"NAMES {channel}")
        msgs = await observer.collect_until("366", timeout=5.0)
        names = [m for m in msgs if m.command == "353"]
        assert names, "Expected NAMES reply"
        joined = " ".join(m.params[-1] for m in names)

        hidden_entry = f"tu71n!testuser@{VIS_HOST}"
        assert hidden_entry in joined, f"Missing untilded hidden NAMES entry: {joined!r}"
        assert f"tu71n!~testuser@" not in joined, joined
    finally:
        for client in (user, observer):
            try:
                await client.send("QUIT :cleanup")
            except Exception:
                pass
            await client.disconnect()


async def test_join_prefix_is_visible_identity(ircd_network):
    """What channel members see in a JOIN prefix is the visible identity."""
    hub = ircd_network["hub"]
    channel = "#tu71_join"

    observer = IRCClient()
    await observer.connect(hub["host"], hub["port"])
    await observer.register("tu71j2", "testuser", "Test User")
    await observer.send(f"JOIN {channel}")
    await observer.wait_for("366")

    user = IRCClient()
    await user.connect(hub["host"], hub["port"])
    await user.register("tu71j", "testuser", "Test User")

    try:
        await user.send(f"JOIN {channel}")
        seen = None
        for _ in range(10):
            msg = await observer.recv(timeout=5.0)
            if msg.command == "JOIN" and msg.prefix and msg.prefix.lower().startswith("tu71j!"):
                seen = msg
                break
        assert seen is not None, "observer never saw the JOIN"
        assert seen.prefix == f"tu71j!testuser@{VIS_HOST}", seen.prefix
    finally:
        for client in (user, observer):
            try:
                await client.send("QUIT :cleanup")
            except Exception:
                pass
            await client.disconnect()


async def test_feature_disabled_keeps_tilde_in_whois(ircd_network):
    """SET TRUST_USERNAME FALSE restores the tilde; the host stays hidden."""
    hub = ircd_network["hub"]

    oper = IRCClient()
    await oper.connect(hub["host"], hub["port"])
    await oper.register("tu71fo", "testuser", "Test User")
    await oper_up(oper)

    user = IRCClient()
    await user.connect(hub["host"], hub["port"])
    await user.register("tu71fu", "testuser", "Test User")

    observer = IRCClient()
    await observer.connect(hub["host"], hub["port"])
    await observer.register("tu71fw", "testuser", "Test User")

    try:
        await oper.send("SET TRUST_USERNAME FALSE")
        set_reply = await oper.wait_for("NOTICE", timeout=5.0)
        assert set_reply.command != "ERR", f"SET failed: {set_reply}"
        await asyncio.sleep(0.3)

        username, host = await whois_userline(observer, "tu71fu")
        assert username == "~testuser", (
            f"With TRUST_USERNAME off, WHOIS should keep tilde: {username!r}"
        )
        assert host == VIS_HOST, host
    finally:
        try:
            await oper.send("SET TRUST_USERNAME TRUE")
            await oper.wait_for("NOTICE", timeout=3.0)
        except Exception:
            pass
        for client in (oper, user, observer):
            try:
                await client.send("QUIT :cleanup")
            except Exception:
                pass
            await client.disconnect()


async def test_who_matches_visible_untilded_username(ircd_network):
    """WHO with the ``u`` match flag matches the untilded visible username."""
    hub = ircd_network["hub"]

    user = IRCClient()
    await user.connect(hub["host"], hub["port"])
    await user.register("tu71who", "whoident", "Test User")

    observer = IRCClient()
    await observer.connect(hub["host"], hub["port"])
    await observer.register("tu71who2", "testuser", "Test User")

    try:
        await observer.send("WHO whoident u")
        msgs = await observer.collect_until("315", timeout=5.0)
        hits = [m for m in msgs if m.command == "352" and m.params[5] == "tu71who"]
        assert hits, f"WHO by untilded username found nothing: {msgs}"
        assert hits[0].params[2] == "whoident", hits[0].params
        assert hits[0].params[3] == VIS_HOST, hits[0].params
    finally:
        for client in (user, observer):
            try:
                await client.send("QUIT :cleanup")
            except Exception:
                pass
            await client.disconnect()
