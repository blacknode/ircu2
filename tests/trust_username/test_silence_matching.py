"""SILENCE matching for TRUST_USERNAME paired identities.

The same two identities a channel ban may match (``~user@realhost`` and
``user@<vhost>``) are what SILENCE matches; a mixed ``user@realhost``
matches nothing.
"""

import asyncio
import pytest

from irc_client import IRCClient

from trust_username.helpers import REAL_HOST, VIS_HOST


pytestmark = pytest.mark.multi_server


async def _silence_check(
    ircd_network,
    *,
    nick_target: str,
    nick_sender: str,
    silence_mask: str,
    expect_silenced: bool,
):
    hub = ircd_network["hub"]

    target = IRCClient()
    await target.connect(hub["host"], hub["port"])
    await target.register(nick_target, "testuser", "Test User")

    sender = IRCClient()
    await sender.connect(hub["host"], hub["port"])
    await sender.register(nick_sender, "testuser", "Test User")

    try:
        mask = silence_mask.format(real_host=REAL_HOST, vis_host=VIS_HOST)
        await target.silence(f"+{mask}")
        await asyncio.sleep(0.3)

        await sender.send(f"PRIVMSG {nick_target} :hello there")
        if expect_silenced:
            await target.assert_no_message("PRIVMSG", timeout=2.0)
        else:
            msg = await target.wait_for("PRIVMSG", timeout=5.0)
            assert msg.params[-1] == "hello there"
    finally:
        for client in (target, sender):
            try:
                await client.send("QUIT :cleanup")
            except Exception:
                pass
            await client.disconnect()


async def test_silence_real_tilded_user_any_host(ircd_network):
    await _silence_check(
        ircd_network, nick_target="tu71s1t", nick_sender="tu71s1s",
        silence_mask="*!~testuser@*", expect_silenced=True,
    )


async def test_silence_real_tilded_user_realhost(ircd_network):
    await _silence_check(
        ircd_network, nick_target="tu71s2t", nick_sender="tu71s2s",
        silence_mask="*!~testuser@{real_host}", expect_silenced=True,
    )


async def test_silence_visible_user_hiddenhost(ircd_network):
    await _silence_check(
        ircd_network, nick_target="tu71s3t", nick_sender="tu71s3s",
        silence_mask="*!testuser@{vis_host}", expect_silenced=True,
    )


async def test_silence_any_user_hiddenhost(ircd_network):
    await _silence_check(
        ircd_network, nick_target="tu71s4t", nick_sender="tu71s4s",
        silence_mask="*!*@{vis_host}", expect_silenced=True,
    )


async def test_silence_rejects_visible_user_realhost(ircd_network):
    await _silence_check(
        ircd_network, nick_target="tu71s5t", nick_sender="tu71s5s",
        silence_mask="*!testuser@{real_host}", expect_silenced=False,
    )


async def test_silence_rejects_wrong_visible_user(ircd_network):
    await _silence_check(
        ircd_network, nick_target="tu71s6t", nick_sender="tu71s6s",
        silence_mask="*!otheruser@{vis_host}", expect_silenced=False,
    )
