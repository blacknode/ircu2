"""Fixtures for the file tests.

The schema and an account come from identity_db/: a file belongs to an
account, and an account is what that suite already knows how to make.
Importing its ``address`` fixture rather than writing one again is what
keeps the two suites saying the same thing about what an account is.

What cannot be imported is the schema, because there are two sets of
migrations here -- identity's, because the tests log in, and filehost's --
and the fixture below is therefore the one ``address`` resolves to: a
fixture is looked up from the test that asks for it, not from the module
it was written in.
"""

import asyncio

import pytest

from irc_client import IRCClient

from identity_db.test_identity_db import (  # noqa: F401  (pytest fixture)
    _drain,
    _oper,
    _quit,
    _read_notices,
    address,
)


#: Which topology generation the schemas were applied to, so that they are
#: applied once per container and not once per test.
_schema_generation = None


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
async def schema(ircd_identity, topology_generation):
    """Both schemas, created the way an operator creates them.

    Two modules, two sets of migrations, one operator running both --
    which is how a deployment does it, and therefore how this does it.
    """
    global _schema_generation

    if _schema_generation != topology_generation:
        client = IRCClient()
        await client.connect(ircd_identity["host"], ircd_identity["port"])
        try:
            await client.register("filemigrator", "oper", "File Migrator")
            await _drain(client)
            await _oper(client)

            await _apply(client, "identity")
            await _apply(client, "filehost")
        finally:
            await _quit(client)

        _schema_generation = topology_generation

    yield
