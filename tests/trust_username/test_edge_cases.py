"""Edge cases around the real vs. visible identity.

G-lines match the real identity only (``~user@realhost``): neither the
untilded visible username nor the hidden host is a G-line target.
"""

import pytest

from irc_client import IRCClient

from trust_username.helpers import (
    VIS_HOST,
    add_gline,
    oper_up,
    remove_gline,
    whois_actual,
)


pytestmark = pytest.mark.multi_server


async def test_gline_matches_tilded_username_real_host(ircd_network):
    """GLINE on ~user@realhost matches a user whose internal username is ~user."""
    hub = ircd_network["hub"]

    oper = IRCClient()
    await oper.connect(hub["host"], hub["port"])
    await oper.register("tu71op", "operuser", "Test User")
    await oper_up(oper)

    victim = IRCClient()
    gline_mask = None
    try:
        await victim.connect(hub["host"], hub["port"])
        # Distinct USER so a leftover G-line cannot poison later tests.
        await victim.register("tu71gv", "glineok", "Test User")

        username, host = await whois_actual(oper, "tu71gv")
        assert username.startswith("~"), f"Expected tilded real user, got {username!r}"

        gline_mask = f"{username}@{host}"
        await add_gline(oper, gline_mask)

        msg = await victim.wait_for("465", timeout=10.0)
        assert msg.command == "465", f"Expected G-line kill, got {msg}"
    finally:
        if gline_mask:
            await remove_gline(oper, gline_mask)
        try:
            await victim.disconnect()
        except Exception:
            pass
        try:
            await oper.send("QUIT :cleanup")
        except Exception:
            pass
        await oper.disconnect()


async def test_gline_does_not_match_untilded_username(ircd_network):
    """GLINE on user@realhost (no tilde) must not match a ~user connection."""
    hub = ircd_network["hub"]

    oper = IRCClient()
    await oper.connect(hub["host"], hub["port"])
    await oper.register("tu72op", "operuser", "Test User")
    await oper_up(oper)

    victim = IRCClient()
    gline_mask = None
    try:
        await victim.connect(hub["host"], hub["port"])
        await victim.register("tu72gv", "glineno", "Test User")

        username, host = await whois_actual(oper, "tu72gv")
        assert username.startswith("~"), f"Expected tilded user, got {username!r}"

        gline_mask = f"glineno@{host}"
        await add_gline(oper, gline_mask)

        await victim.send("PING :alive")
        pong = await victim.wait_for("PONG", timeout=5.0)
        assert pong.command == "PONG", (
            f"Untilded GLINE {gline_mask!r} should not match ~user"
        )
    finally:
        if gline_mask:
            await remove_gline(oper, gline_mask)
        try:
            await victim.send("QUIT :cleanup")
        except Exception:
            pass
        await victim.disconnect()
        try:
            await oper.send("QUIT :cleanup")
        except Exception:
            pass
        await oper.disconnect()


async def test_gline_does_not_match_hidden_host(ircd_network):
    """GLINE on the hidden host must not match: G-lines see real hosts only."""
    hub = ircd_network["hub"]

    oper = IRCClient()
    await oper.connect(hub["host"], hub["port"])
    await oper.register("tu73op", "operuser", "Test User")
    await oper_up(oper)

    victim = IRCClient()
    gline_mask = None
    try:
        await victim.connect(hub["host"], hub["port"])
        await victim.register("tu73gv", "glinevh", "Test User")

        gline_mask = f"~glinevh@{VIS_HOST}"
        await add_gline(oper, gline_mask)

        await victim.send("PING :alive")
        pong = await victim.wait_for("PONG", timeout=5.0)
        assert pong.command == "PONG", (
            f"GLINE on the hidden host {gline_mask!r} should not match"
        )
    finally:
        if gline_mask:
            await remove_gline(oper, gline_mask)
        try:
            await victim.send("QUIT :cleanup")
        except Exception:
            pass
        await victim.disconnect()
        try:
            await oper.send("QUIT :cleanup")
        except Exception:
            pass
        await oper.disconnect()
