"""Translations from PO catalogs (doc/proposals/005-traducciones-po.md).

The image installs po/es.po as the core catalog, so a client that asks
for Spanish with LANGUAGE -- before or after registering -- gets its
numerics translated, and everything else stays in English.  The
preference travels the network as the LG token, which is what makes a
reply generated on another server come out right.
"""

from __future__ import annotations

import asyncio

import pytest

from cap_helpers import connect_services, make_cap_client, oper_up
from irc_client import IRCClient
from p10_server import server_numeric, strip_msg_tags


async def _cap_ls_tokens(client: IRCClient) -> list[str]:
    tokens: list[str] = []
    while True:
        msg = await client.wait_for("CAP", timeout=5.0)
        assert msg.params[1] == "LS", msg.raw
        tokens.extend(msg.params[-1].split())
        if len(msg.params) < 4 or msg.params[2] != "*":
            return tokens


@pytest.mark.single_server
async def test_cap_ls_302_advertises_languages(ircd_hub):
    """CAP LS 302 lists draft/languages with the max and the languages."""
    client = IRCClient()
    await client.connect(ircd_hub["host"], ircd_hub["port"])
    try:
        await client.send("CAP LS 302")
        tokens = await _cap_ls_tokens(client)
        langs = [t for t in tokens if t.startswith("draft/languages=")]
        assert langs, tokens
        value = langs[0].split("=", 1)[1].split(",")
        assert value[0] == "3", value
        assert "en" in value and "es" in value, value
    finally:
        await client.send("QUIT :done")
        await client.disconnect()


@pytest.mark.single_server
async def test_cap_ls_301_has_no_value(ircd_hub):
    """Without LS 302 the capability is listed bare."""
    client = IRCClient()
    await client.connect(ircd_hub["host"], ircd_hub["port"])
    try:
        await client.send("CAP LS")
        tokens = await _cap_ls_tokens(client)
        assert "draft/languages" in tokens, tokens
        assert not any(t.startswith("draft/languages=") for t in tokens)
    finally:
        await client.send("QUIT :done")
        await client.disconnect()


@pytest.mark.single_server
async def test_language_before_registration_translates_welcome(ircd_hub):
    """LANGUAGE es before NICK/USER: 687 in Spanish, then a Spanish 001."""
    client = IRCClient()
    await client.connect(ircd_hub["host"], ircd_hub["port"])
    try:
        await client.send("LANGUAGE es")
        msg = await client.wait_for("687", timeout=5.0)
        assert msg.params[0] == "*", msg.raw
        assert msg.params[1] == "es", msg.raw
        assert "preferencias de idioma" in msg.params[-1], msg.raw

        msgs = await client.register("hola", "hola", "Hola")
        welcome = next(m for m in msgs if m.command == "001")
        assert welcome.params[-1].startswith("Bienvenido"), welcome.raw
        # Lusers and MOTD end come translated too.
        end = next(m for m in msgs if m.command in ("376", "422"))
        assert "Fin del comando /MOTD" in end.params[-1] or \
            "No hay fichero de MOTD" in end.params[-1], end.raw
    finally:
        await client.send("QUIT :done")
        await client.disconnect()


@pytest.mark.single_server
async def test_language_after_registration_and_back(ircd_hub):
    """A registered user switches to Spanish and back to English."""
    client = await make_cap_client(ircd_hub["host"], ircd_hub["port"], "lang1")
    try:
        msg = await client.send_and_expect("WHOIS nadie", "401")
        assert msg.params[-1] == "No such nick", msg.raw

        msg = await client.send_and_expect("LANGUAGE es", "687")
        assert msg.params[1] == "es", msg.raw
        msg = await client.send_and_expect("WHOIS nadie", "401")
        assert msg.params[-1] == "No existe ese nick", msg.raw

        # WHOIS of oneself shows the preference.
        await client.send("WHOIS lang1")
        msg = await client.wait_for("690", timeout=5.0)
        assert msg.params[1] == "lang1" and msg.params[2] == "es", msg.raw
        await client.wait_for("318", timeout=5.0)

        msg = await client.send_and_expect("LANGUAGE en", "687")
        assert "Language preferences" in msg.params[-1], msg.raw
        msg = await client.send_and_expect("WHOIS nadie", "401")
        assert msg.params[-1] == "No such nick", msg.raw
    finally:
        await client.send("QUIT :done")
        await client.disconnect()


@pytest.mark.single_server
async def test_language_errors(ircd_hub):
    """982 names the unknown codes and changes nothing; 981 for too many;
    461 for none."""
    client = await make_cap_client(ircd_hub["host"], ircd_hub["port"], "lang2")
    try:
        msg = await client.send_and_expect("LANGUAGE xx", "982")
        assert msg.params[1] == "xx", msg.raw

        # Only the unknown ones are listed, and the known one is not set.
        msg = await client.send_and_expect("LANGUAGE es zz-Latn yy", "982")
        assert msg.params[1:3] == ["zz-Latn", "yy"], msg.raw
        msg = await client.send_and_expect("WHOIS nadie", "401")
        assert msg.params[-1] == "No such nick", msg.raw

        msg = await client.send_and_expect("LANGUAGE es en es-MX pt", "981")
        assert msg.params[1] == "3", msg.raw

        msg = await client.send_and_expect("LANGUAGE", "461")
        assert msg.params[1] == "LANGUAGE", msg.raw

        # A regional variant of a catalog is accepted through its primary
        # subtag, and en-GB is the source language.
        msg = await client.send_and_expect("LANGUAGE es-AR en-GB", "687")
        assert msg.params[1:3] == ["es-AR", "en-GB"], msg.raw
        msg = await client.send_and_expect("WHOIS nadie", "401")
        assert msg.params[-1] == "No existe ese nick", msg.raw
    finally:
        await client.send("QUIT :done")
        await client.disconnect()


@pytest.mark.single_server
async def test_only_the_recipient_is_translated(ircd_hub):
    """A Spanish speaker's choice does not leak into anybody else's replies."""
    es = await make_cap_client(ircd_hub["host"], ircd_hub["port"], "castell")
    en = await make_cap_client(ircd_hub["host"], ircd_hub["port"], "english")
    try:
        await es.send_and_expect("LANGUAGE es", "687")
        msg = await es.send_and_expect("WHOIS nadie", "401")
        assert msg.params[-1] == "No existe ese nick", msg.raw
        msg = await en.send_and_expect("WHOIS nadie", "401")
        assert msg.params[-1] == "No such nick", msg.raw
        # An unregistered-only numeric is not the point; a plain PRIVMSG
        # between them is not touched either.
        await es.send("PRIVMSG english :hola")
        msg = await en.wait_for("PRIVMSG", timeout=5.0)
        assert msg.params[-1] == "hola", msg.raw
        # Another user does not see the 690 of the Spanish speaker.
        await en.send("WHOIS castell")
        msgs = await en.collect_until("318", timeout=5.0)
        assert not any(m.command == "690" for m in msgs), [m.raw for m in msgs]
    finally:
        await es.send("QUIT :done")
        await en.send("QUIT :done")
        await es.disconnect()
        await en.disconnect()


@pytest.mark.single_server
async def test_oper_sees_690_and_stats_n(ircd_hub):
    """An oper sees the preference in WHOIS and the catalogs in /STATS n."""
    es = await make_cap_client(ircd_hub["host"], ircd_hub["port"], "castel2")
    op = await make_cap_client(ircd_hub["host"], ircd_hub["port"], "langop")
    try:
        await es.send_and_expect("LANGUAGE es-MX es", "687")
        await oper_up(op)

        await op.send("WHOIS castel2")
        msg = await op.wait_for("690", timeout=5.0)
        assert msg.params[1] == "castel2", msg.raw
        assert msg.params[2:4] == ["es-mx", "es"], msg.raw
        await op.wait_for("318", timeout=5.0)

        await op.send("STATS n")
        msgs = await op.collect_until("219", timeout=5.0)
        lines = [m.params[-1] for m in msgs if m.command == "249"]
        assert any(l.startswith("core es:") and "0 rejected" in l for l in lines), lines
    finally:
        await es.send("QUIT :done")
        await op.send("QUIT :done")
        await es.disconnect()
        await op.disconnect()


@pytest.mark.single_server
async def test_lg_token_on_the_wire(ircd_hub):
    """LG follows a LANGUAGE change and rides the burst behind the NICK;
    an LG received from a server is honoured in replies the hub makes."""
    user = await make_cap_client(ircd_hub["host"], ircd_hub["port"], "lgburst")
    await user.send_and_expect("LANGUAGE es-AR es", "687")

    srv = await connect_services(ircd_hub)
    try:
        burst = [strip_msg_tags(l) for l in srv.received]
        numnick = srv.get_user_numnick("lgburst")
        assert numnick is not None
        n_at = next(i for i, l in enumerate(burst) if l.split()[1:3] == ["N", "lgburst"])
        after = burst[n_at + 1:]
        assert after and after[0] == f"{numnick} LG es-ar es", burst[n_at:n_at + 3]
        assert sum(1 for l in burst if " LG " in l) == 1, "one LG per user with a preference"

        # A change on a registered user propagates.
        await user.send_and_expect("LANGUAGE en", "687")
        lines = await srv.recv_until("LG", timeout=5.0)
        assert strip_msg_tags(lines[-1]) == f"{numnick} LG en", lines[-1]

        # A remote user's LG is stored and used: a WHOIS aimed at the hub is
        # answered by the hub, in Spanish.
        hub_num = server_numeric(1)
        bob = await srv.introduce_user("bob", username="bob", host="remote.test.net")
        await srv._send(f"{bob} LG es")
        await srv.drain_messages(0.3)
        await srv._send(f"{bob} W {hub_num} :nadie")
        lines = await srv.recv_until("401", timeout=5.0)
        line = strip_msg_tags(lines[-1])
        assert line.endswith("nadie :No existe ese nick"), line

        # Junk in an LG is dropped, the valid codes are kept.
        await srv._send(f"{bob} LG not/a/code en")
        await srv.drain_messages(0.3)
        await srv._send(f"{bob} W {hub_num} :nadie")
        lines = await srv.recv_until("401", timeout=5.0)
        line = strip_msg_tags(lines[-1])
        assert line.endswith("nadie :No such nick"), line
    finally:
        await srv.disconnect()
        await user.send("QUIT :done")
        await user.disconnect()


@pytest.mark.multi_server
async def test_remote_reply_is_translated_across_a_real_link(ircd_network):
    """A leaf user with LANGUAGE es gets Spanish from the hub for a WHOIS
    the hub answers.  A remote WHOIS is an oper's under HIS_REMOTE, so the
    asker opers up first; the 318 then comes from the hub, translated for
    the asker because the LG reached it."""
    leaf = ircd_network["leaf1"]
    hub = ircd_network["hub"]
    target = await make_cap_client(hub["host"], hub["port"], "enelhub")
    client = await make_cap_client(leaf["host"], leaf["port"], "lejano")
    try:
        await oper_up(client)
        await client.send_and_expect("LANGUAGE es", "687")
        await asyncio.sleep(0.5)  # let the LG reach the hub
        msg = await client.send_and_expect("WHOIS enelhub enelhub", "318")
        assert msg.prefix == hub["name"], msg.raw
        assert msg.params[-1] == "Fin de la lista de /WHOIS.", msg.raw
        msg = await client.send_and_expect(f"WHOIS {hub['name']} nadie", "401")
        assert msg.prefix == hub["name"], msg.raw
        assert msg.params[-1] == "No existe ese nick", msg.raw
        await client.wait_for("318", timeout=5.0)  # the end of that one

        await client.send_and_expect("LANGUAGE en", "687")
        await asyncio.sleep(0.5)
        msg = await client.send_and_expect("WHOIS enelhub enelhub", "318")
        assert msg.prefix == hub["name"], msg.raw
        assert msg.params[-1] == "End of /WHOIS list.", msg.raw

        # The hub user, who asked for nothing, is answered in English by
        # its own server, whatever the oper chose.
        msg = await target.send_and_expect("WHOIS lejano", "318")
        assert msg.prefix == hub["name"], msg.raw
        assert msg.params[-1] == "End of /WHOIS list.", msg.raw
    finally:
        await client.send("QUIT :done")
        await target.send("QUIT :done")
        await client.disconnect()
        await target.disconnect()
