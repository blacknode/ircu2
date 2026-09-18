"""Channel-ban matching for TRUST_USERNAME paired identities.

A hidden user has two complete identities a ban may match:

  ~user@realhost   (real: what the server knows and operators see)
  user@<vhost>     (visible: what everyone else sees)

Mixed forms such as user@realhost must not match.  Every user is hidden
from registration on, so there is no "before" state any more; the real
host is what an operator's WHOIS (338) reports.
"""

import asyncio
import pytest

from irc_client import IRCClient

from trust_username.helpers import (
    REAL_HOST,
    VIS_HOST,
    oper_up,
    whois_actual,
    whois_userline,
)


pytestmark = pytest.mark.multi_server


async def _ban_join_check(
    ircd_network,
    *,
    channel: str,
    nick_op: str,
    nick_victim: str,
    ban_mask: str,
    expect_banned: bool,
):
    """Set ban_mask, have the victim JOIN, assert 474 or 366."""
    hub = ircd_network["hub"]

    chanop = IRCClient()
    await chanop.connect(hub["host"], hub["port"])
    await chanop.register(nick_op, "testuser", "Test User")
    await oper_up(chanop)

    victim = IRCClient()
    await victim.connect(hub["host"], hub["port"])
    await victim.register(nick_victim, "testuser", "Test User")

    try:
        await chanop.send(f"JOIN {channel}")
        await chanop.wait_for("366")

        real_user, real_host = await whois_actual(chanop, nick_victim)
        assert real_user == "~testuser", real_user
        assert real_host == REAL_HOST, real_host
        vis_user, vis_host = await whois_userline(chanop, nick_victim)
        assert vis_user == "testuser", f"Expected visible untilded user, got {vis_user!r}"
        assert vis_host == VIS_HOST, vis_host

        mask = ban_mask.format(real_host=real_host, vis_host=vis_host)
        await chanop.send(f"MODE {channel} +b {mask}")
        await chanop.wait_for("MODE")
        await asyncio.sleep(0.3)

        await victim.send(f"JOIN {channel}")
        if expect_banned:
            msg = await victim.wait_for("474", timeout=5.0)
            assert msg.command == "474", f"Expected ban for {mask!r}, got: {msg}"
        else:
            msg = await victim.wait_for("366", timeout=5.0)
            assert msg.command == "366", (
                f"Expected JOIN success for {mask!r}, got: {msg}"
            )
        return mask, real_host, vis_host
    finally:
        for client in (chanop, victim):
            try:
                await client.send("QUIT :cleanup")
            except Exception:
                pass
            await client.disconnect()


# --- Should match: real identity ---

async def test_ban_real_tilded_user_any_host(ircd_network):
    await _ban_join_check(
        ircd_network, channel="#tu71_b1", nick_op="tu71b1o", nick_victim="tu71b1v",
        ban_mask="*!~testuser@*", expect_banned=True,
    )


async def test_ban_real_tilded_user_realhost(ircd_network):
    await _ban_join_check(
        ircd_network, channel="#tu71_b2", nick_op="tu71b2o", nick_victim="tu71b2v",
        ban_mask="*!~testuser@{real_host}", expect_banned=True,
    )


async def test_ban_real_tilded_user_hiddenhost(ircd_network):
    """A tilded user with the hidden host also matches: the visible host is
    the user's host as far as the channel is concerned."""
    await _ban_join_check(
        ircd_network, channel="#tu71_b3", nick_op="tu71b3o", nick_victim="tu71b3v",
        ban_mask="*!~testuser@{vis_host}", expect_banned=True,
    )


# --- Should match: visible identity ---

async def test_ban_visible_user_any_host(ircd_network):
    await _ban_join_check(
        ircd_network, channel="#tu71_b4", nick_op="tu71b4o", nick_victim="tu71b4v",
        ban_mask="*!testuser@*", expect_banned=True,
    )


async def test_ban_visible_user_hiddenhost(ircd_network):
    await _ban_join_check(
        ircd_network, channel="#tu71_b5", nick_op="tu71b5o", nick_victim="tu71b5v",
        ban_mask="*!testuser@{vis_host}", expect_banned=True,
    )


async def test_ban_any_user_hiddenhost(ircd_network):
    await _ban_join_check(
        ircd_network, channel="#tu71_b6", nick_op="tu71b6o", nick_victim="tu71b6v",
        ban_mask="*!*@{vis_host}", expect_banned=True,
    )


async def test_ban_any_user_realhost(ircd_network):
    """A ban an operator places on the real host still lands."""
    await _ban_join_check(
        ircd_network, channel="#tu71_b6r", nick_op="tu71b6ro", nick_victim="tu71b6rv",
        ban_mask="*!*@{real_host}", expect_banned=True,
    )


async def test_ban_nick_visible_user_hiddenhost(ircd_network):
    await _ban_join_check(
        ircd_network, channel="#tu71_b7", nick_op="tu71b7o", nick_victim="tu71b7v",
        ban_mask="tu71b7v!testuser@{vis_host}", expect_banned=True,
    )


# --- Should NOT match ---

async def test_ban_rejects_visible_user_realhost(ircd_network):
    """user@realhost is a mixed identity: nobody is that."""
    await _ban_join_check(
        ircd_network, channel="#tu71_b8", nick_op="tu71b8o", nick_victim="tu71b8v",
        ban_mask="*!testuser@{real_host}", expect_banned=False,
    )


async def test_ban_rejects_wrong_visible_user(ircd_network):
    await _ban_join_check(
        ircd_network, channel="#tu71_b9", nick_op="tu71b9o", nick_victim="tu71b9v",
        ban_mask="*!otheruser@{vis_host}", expect_banned=False,
    )


async def test_ban_rejects_wrong_hidden_host(ircd_network):
    await _ban_join_check(
        ircd_network, channel="#tu71_b10", nick_op="tu71b10o", nick_victim="tu71b10v",
        ban_mask="*!testuser@AAAAAA.AAAAAA.v4", expect_banned=False,
    )


async def test_ban_rejects_unrelated_nick(ircd_network):
    await _ban_join_check(
        ircd_network, channel="#tu71_b11", nick_op="tu71b11o", nick_victim="tu71b11v",
        ban_mask="othernick!testuser@{vis_host}", expect_banned=False,
    )


async def test_ban_applies_to_existing_member_visible_mask(ircd_network):
    """A ban on the visible mask silences a member already in the channel."""
    hub = ircd_network["hub"]
    channel = "#tu71_b12"

    chanop = IRCClient()
    await chanop.connect(hub["host"], hub["port"])
    await chanop.register("tu71b12o", "testuser", "Test User")

    victim = IRCClient()
    await victim.connect(hub["host"], hub["port"])
    await victim.register("tu71b12v", "testuser", "Test User")

    try:
        await chanop.send(f"JOIN {channel}")
        await chanop.wait_for("366")
        await victim.send(f"JOIN {channel}")
        await victim.wait_for("366")

        await chanop.send(f"MODE {channel} +b *!testuser@{VIS_HOST}")
        await chanop.wait_for("MODE")
        await asyncio.sleep(0.3)

        await victim.send(f"PRIVMSG {channel} :should be blocked")
        msg = await victim.wait_for("404", timeout=5.0)
        assert msg.command == "404", f"Expected ERR_CANNOTSENDTOCHAN, got: {msg}"
    finally:
        for client in (chanop, victim):
            try:
                await client.send("QUIT :cleanup")
            except Exception:
                pass
            await client.disconnect()
