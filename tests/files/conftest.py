"""Fixtures for the file tests.

A file belongs to an account, and an account is what the network's
services say it is: ``store_services`` (tests/conftest.py) is the U:lined
P10 server that logs a client in, the way doc/readme.accounting describes.

The schema is the file host's own, created the way an operator creates
it -- ``/MODULE MIGRATION APPLY filehost``.
"""

import asyncio

import pytest

from irc_client import IRCClient


#: Which topology generation the schema was applied to, so that it is
#: applied once per container and not once per test.
_schema_generation = None


async def _drain(client, timeout=1.0):
    """Read until the stream goes quiet."""
    loop = asyncio.get_running_loop()
    deadline = loop.time() + timeout
    while loop.time() < deadline:
        try:
            await client.recv(timeout=0.4)
        except (asyncio.TimeoutError, TimeoutError):
            return


async def _oper(client):
    await client.send("OPER testoper operpass")
    await client.wait_for("381", timeout=10.0)


async def _quit(client):
    try:
        await client.send("QUIT :done")
    except Exception:
        pass
    await client.disconnect()


async def _read_notices(client, timeout=10.0):
    """Every NOTICE until the stream goes quiet."""
    lines = []
    loop = asyncio.get_running_loop()
    deadline = loop.time() + timeout

    while loop.time() < deadline:
        try:
            msg = await client.recv(timeout=1.0)
        except (asyncio.TimeoutError, TimeoutError):
            if lines:
                return lines
            continue
        if msg.command == "NOTICE":
            lines.append(msg.params[-1])

    return lines


async def _apply(client, module):
    """Run one module's migrations and wait until none are pending.

    APPLY is idempotent and asynchronous -- the migrations run one
    connection at a time and the replies arrive as they finish -- so what
    is believed is what STATUS says afterwards, not the running commentary.
    """
    await client.send(f"MODULE MIGRATION APPLY {module}")
    await _drain(client, timeout=2.0)

    deadline = asyncio.get_running_loop().time() + 90.0
    status = ""
    while asyncio.get_running_loop().time() < deadline:
        await client.send(f"MODULE MIGRATION STATUS {module}")
        lines = await _read_notices(client, timeout=20.0)
        status = " ".join(lines)
        if status and "pending" not in status:
            break
        await asyncio.sleep(1.0)

    assert status, f"MODULE MIGRATION STATUS {module} said nothing"
    assert "pending" not in status, status
    assert "applied" in status, status


@pytest.fixture
async def schema(ircd_store, topology_generation):
    """The file host's schema, created the way an operator creates it."""
    global _schema_generation

    if _schema_generation != topology_generation:
        client = IRCClient()
        await client.connect(ircd_store["host"], ircd_store["port"])
        try:
            await client.register("filemigrator", "oper", "File Migrator")
            await _drain(client)
            await _oper(client)

            await _apply(client, "filehost")
        finally:
            await _quit(client)

        _schema_generation = topology_generation

    yield
