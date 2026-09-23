"""Files: an upload over HTTP, a link, and what happens to it afterwards.

The whole path, and it is a path with three parts that have to agree: the
ircd mints a ticket, an ordinary HTTP client sends the bytes, and the
bytes come back out of a URL.  None of it goes through the event loop --
the worker writes the body to the spool as it arrives and streams the
file back out -- so what these tests are really pinning down is that a
file bigger than anything the server would hold in memory survives the
round trip unchanged.

The topology is the store one (marker ``store``): the module keeps its
metadata in PostgreSQL, and a file belongs to an account, so the tests
also need the network's services to log a client in.
"""

import asyncio
import hashlib
import os
import re
import urllib.error
import urllib.request
import uuid

import pytest

from irc_client import IRCClient

pytestmark = pytest.mark.store

#: A token unique to this run, put in every nickname and account name.
#: PostgreSQL keeps what a previous run uploaded, and a listing is by
#: account, so a fixed account name would read back somebody else's files.
TAG = uuid.uuid4().hex[:6]

#: What FILE UPLOAD tells the client, in the line with the command in it.
#:
#: The ticket is inside the single quotes of the header argument, so the
#: quote is where it ends: a \S+ would take it with it.
TICKET_RE = re.compile(r"Bearer ([^']+)' (\S+)/u/(\S+)")
#: And the line with the eventual link.
LINK_RE = re.compile(r"It will then be at (\S+)")


async def _connect(hub, nick):
    client = IRCClient()
    await client.connect(hub["host"], hub["port"])
    await client.register(nick, "testuser", "File Test")
    await client.drain()
    return client


async def _oper(client):
    await client.send("OPER testoper operpass")
    await client.wait_for("381", timeout=10.0)


async def _notices(client, timeout=10.0):
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


def _http(method, url, data=None, headers=None):
    """One HTTP request, synchronously.  Returns (status, body)."""
    req = urllib.request.Request(url, data=data, method=method,
                                 headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as err:
        return err.code, err.read()


@pytest.fixture
async def uploader(ircd_store, schema, store_services):
    """A client identified to an account, which is what uploading needs.

    The account comes from the services, as it does on a real network:
    an ACCOUNT from the U:lined server, and nothing this server decided.
    """
    nick = f"up{TAG}"
    client = await _connect(ircd_store, nick)
    await store_services.send_register(nick, account=f"acct{TAG}")

    loop = asyncio.get_running_loop()
    deadline = loop.time() + 10.0
    while True:
        await client.send(f"MODE {nick}")
        msg = await client.wait_for("221", timeout=5.0)
        if "r" in msg.params[-1]:
            break
        assert loop.time() < deadline, f"{nick} never got an account"
        await asyncio.sleep(0.2)
    await client.drain()

    yield client

    try:
        await client.send("QUIT :done")
    except Exception:
        pass
    await client.disconnect()


async def _ticket(client, target=""):
    """Ask for an upload ticket; return (ticket, upload_url, link)."""
    await client.send(f"FILE UPLOAD {target}".strip())
    lines = await _notices(client)

    joined = "\n".join(lines)
    found = TICKET_RE.search(joined)
    link = LINK_RE.search(joined)

    assert found, joined
    assert link, joined

    ticket, base, file_id = found.groups()

    return ticket, f"{base}/u/{file_id}", link.group(1)


async def test_upload_and_download(ircd_store, uploader):
    """A file goes up, comes back byte for byte, and is where it said."""
    ticket, upload_url, link = await _ticket(uploader, "#files")

    # Bigger than HTTP_BODY_MAX (64 KiB), so it is the streaming path and
    # not the ordinary one: this is the case the whole design is for.
    payload = os.urandom(200 * 1024)
    digest = hashlib.sha256(payload).hexdigest()

    status, body = _http("PUT", upload_url, payload, {
        "Authorization": f"Bearer {ticket}",
        "Content-Type": "application/octet-stream",
        "X-File-Name": "holiday.bin",
    })

    assert status == 201, body
    assert body.decode().strip() == link

    status, got = _http("GET", link)

    assert status == 200
    assert hashlib.sha256(got).hexdigest() == digest
    assert len(got) == len(payload)


async def test_a_ticket_is_for_one_upload(ircd_store, uploader):
    """The identifier is in the ticket as well as in the path.

    A ticket that worked for any identifier would be a ticket to overwrite
    somebody else's file.
    """
    ticket, upload_url, _link = await _ticket(uploader)
    other = upload_url[:-16] + "0123456789abcdef"

    status, body = _http("PUT", other, b"x" * 100, {
        "Authorization": f"Bearer {ticket}",
    })

    assert status == 403, body


async def test_no_ticket_no_upload(ircd_store, uploader):
    """Without one, or with one this network never signed."""
    _ticket_value, upload_url, _link = await _ticket(uploader)

    status, _body = _http("PUT", upload_url, b"x" * 100, {})
    assert status == 403

    status, _body = _http("PUT", upload_url, b"x" * 100, {
        "Authorization": "Bearer 1.YWJjZGVm.YWJjZGVmZ2hpamtsbW5vcA==",
    })
    assert status == 403


async def test_an_unknown_file_is_not_found(ircd_store, uploader):
    """And an identifier that is not one, which must not reach a path."""
    base = (await _ticket(uploader))[2]
    root = base[:base.rindex("/f/")]

    for bad in ("0123456789abcdef", "../../etc/passwd", "short"):
        status, _body = _http("GET", f"{root}/f/{bad}")
        assert status in (400, 404), bad


async def test_list_and_delete(ircd_store, uploader):
    """What an account is holding, and taking one back."""
    ticket, upload_url, link = await _ticket(uploader)
    file_id = link.rsplit("/", 1)[1]

    status, _body = _http("PUT", upload_url, b"a small one", {
        "Authorization": f"Bearer {ticket}",
        "X-File-Name": "note.txt",
    })
    assert status == 201

    await uploader.send("FILE LIST")
    lines = await _notices(uploader)

    assert any(file_id in line for line in lines), lines
    assert any("note.txt" in line for line in lines), lines

    await uploader.send(f"FILE DELETE {file_id}")
    lines = await _notices(uploader)

    assert any("gone" in line for line in lines), lines

    # And it really is: the row went and the object went with it.
    status, _body = _http("GET", link)
    assert status == 404


async def test_uploading_needs_an_account(ircd_store, schema):
    """A bare nickname is not a handle on a person.

    Whoever wears it next week is not whoever uploaded this, and the
    quota, the listing and the deletion all hang off that name.
    """
    client = await _connect(ircd_store, "nofileacct")
    try:
        await client.send("FILE UPLOAD")
        lines = await _notices(client)

        assert any("Identify" in line for line in lines), lines
    finally:
        await client.send("QUIT :done")
        await client.disconnect()
