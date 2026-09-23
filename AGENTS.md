# AGENTS.md

This file provides guidance to Codex when working with code in this repository.

## What this is

ircu2 — the Undernet IRC server (P10 protocol), a single-process C99 daemon.
The IRC core is single-threaded; optional worker threads run auxiliary work
beside it (`include/worker.h`). The historical autotools build has been
replaced by CMake.

## Build

Requires CMake 3.16+, a C99 compiler, bison, and optionally OpenSSL/GnuTLS/libtls.
In-source builds are rejected on purpose.

```sh
cmake -B build -DIRCU_DOMAIN=example.com   # RelWithDebInfo, TLS autodetected
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/ircd/ircd -v                       # smoke test
```

Presets (CMake 3.21+) in `CMakePresets.json`: `dev` (Debug + `IRCU_ENABLE_DEBUG`
+ `-Wall`), `asan`, `tsan`, `release`, `no-tls`, `default`. Use them for anything
non-trivial:

```sh
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

Every old `configure` switch has an `IRCU_*` cache variable (`cmake/IrcuOptions.cmake`,
`INSTALL`): `IRCU_TLS` (openssl/gnutls/libtls/none), `IRCU_ENABLE_IPV6`,
`IRCU_ENABLE_{POLL,EPOLL,KQUEUE,DEVPOLL}`, `IRCU_ENABLE_ASSERTS`, `IRCU_MAXCON`,
`IRCU_DPATH/CPATH/LPATH/SPATH/MPATH`, `IRCU_CHROOT`. `IRCU_ENABLE_ASSERTS` — not
`CMAKE_BUILD_TYPE` — controls `NDEBUG`; the build strips CMake's own `-DNDEBUG`
so it cannot override that.

CI (`.github/workflows/build.yml`) builds the openssl/gnutls/none × RelWithDebInfo
matrix plus one Debug build, runs ctest, and checks that every `.so` under
`build/modules/` resolves only symbols the ircd exports.

## Tests

Two independent suites.

**Unit tests** — C, in `ircd/test/`, registered with CTest. `add_ircu_test(<name>
[sources...])` in `ircd/test/CMakeLists.txt` compiles `<name>.c` + `test_stub.c`
(logging/client stubs) plus whichever `ircd/*.c` files it exercises; there is no
link against the full server. Run one with `ctest --test-dir build -R ircd_match_t`
or by executing `build/ircd/test/<name>` directly. `worker_t` is the only test
with real threads: run it under the `tsan` preset after any change to
`ircd/worker.c`.

**Integration tests** — Python/pytest over Docker, in `tests/` (see `tests/README.md`).

```sh
cd tests && uv sync
uv run pytest                       # everything; brings up containers itself
uv run pytest pr61_uhnames/         # one directory
uv run pytest test_irc_client.py    # unit tests, no Docker
uv run pytest -m single_server      # by topology marker
```

Docker topologies (hub-only, full network, TLS, limits, **store**, DNS,
standalone TLS hub, NETWORK_FEATURES compat) share one compose project and are
mutually exclusive; `tests/conftest.py` groups tests by topology at collection
time, so any selection is safe. Markers are declared in `tests/pyproject.toml`.
Regenerate the test PKI with `tests/docker/generate-certs.sh`. The `store`
topology is one ircd plus a PostgreSQL (`tests/history/`,
`tests/conversation/`, `tests/files/`); the image ships every module under
`/opt/ircu/lib/modules`, and the tests create the schemas themselves with
`/MODULE MIGRATION APPLY history` (and `filehost`). Accounts there come
from the `store_services` fixture — `tests/p10_server.py` linked as the
U:lined `services.test.net`, sending `ACCOUNT` — because this server has
no accounts of its own.

## Architecture

**Event loop.** `ircd/ircd.c` runs the one thread that touches core state.
`include/ircd_events.h` defines the
generic `Socket`/`Timer`/`Signal` generators; each `ircd/engine_*.c` (epoll, kqueue,
devpoll, poll, select) is one backend, picked at configure time. `ircd/s_bsd.c` and
`ircd/listener.c` sit on top for connections; `ircd/packet.c` feeds complete lines
into the parser. Nothing on this thread may block — a stall delays every client;
blocking work goes to a worker (below).

**Command dispatch.** `ircd/parse.c` holds `msgtab[]` (`struct Message`, defined in
`include/msg.h`) and builds two prefix trees: one for full command names, one for
P10 tokens. Each command carries five handlers indexed by `enum HandlerType`
(`include/ircd_handler.h`): unregistered, client, server, oper, service — so the
same command behaves differently by source. Command implementations are the
`ircd/m_*.c` files, one per command; `ircd/m_tmpl.c` is the template. Commands can
also be registered at runtime via `parse_add_command()`.

**User modes.** Bits of a `flag_t` mask in `cli_uflags()` (`include/user_flags.h`),
registered in a run-time list in `ircd/client.c` (`client_user_modes()` and friends)
so modules can add their own; test them with the named `Is*`/`Send*` macros in
`include/client.h`, and a saved pre-change snapshot with the matching `Was*` ones.
All changes go through `do_user_mode()` in `ircd/s_user.c` (`set_user_mode()`
for a user's own modes, `set_user_mode_on()` for a server or `+S` bot changing
somebody else's — `m_mode.c` decides who may reach it).

**Accounting** (`doc/readme.accounting`). The classic ircu model, which
this fork rolled back to: **an account is not a nickname**, and ircu does
not manage the lifecycle of one. The network's services -- a separate
program on a link of its own, U:lined -- keep the accounts and say who is
logged in to what; this server records it and passes it on.
`cli_user()->account` is the name (`ACCOUNTLEN` = 12), with `acc_id` and
`acc_flags` beside it, and `IsAccount()` tests umode `+r`. It arrives two
ways and only two: **`ACCOUNT` (token `AC`) from a U:lined server**
(`ircd/m_account.c`, which checks the `Uworld{}` block against the
originator *and* the uplink, sets the flag, lifts `+f`, fans out
account-notify and relays on), or **`+r <account>[:<id>[:<flags>]]` in a
NICK burst** (`do_user_mode()` reads it, `umode_str()` writes it).
**There is no `-r`** -- the case acts on `UMODE_ADD` alone, because this
server cannot take away what it did not give -- and **a nick change keeps
the account**, since the two are different names for different things.
WHOIS reports it as **330**. `account-notify`, `extended-join` and WHOX
`%a`/`%A` are back with it; `account-tag` is not. Every user is still
`+x` from `register_user()` on and cannot remove it: `hide_hostmask()`
derives the visible host from the IP with the TEA cipher in
`ircd/ircd_vhost.c` (`xxxxxx.yyyyyy.v4|v6`, unit-tested against
IRC-Hispano vectors in `vhost_t`) under the mandatory `Security {
virtual_host_key = "<12 base64 chars>"; }` block, which must be identical
on every server; bots (`+B`/`+S`) keep their configured host. **An
account never changes the host** -- upstream rewrote it to
`<account>.<HIDDEN_HOST>` on login and this does not, so logging in
changes what a user IS called and never what it LOOKS like. **No `SVS*`
commands**: a command that is one network's policy goes through the
module API.

**SASL is relayed, never answered** (`include/sasl.h`, `ircd/sasl.c`,
`ircd/m_sasl.c`, `ircd/m_xreply.c`). One exchange per connection, keyed
by a routing cookie in `cli_sasl()`, forwarded to the services server as
`XQ <numnick> sasl:<cookie> :SASL ...` and answered by an `XREPLY` with
the same prefix (`doc/readme.xquery`). The account in the answer is
applied by `auth_set_account()` in `s_auth.c`, which parses
`<account>[:<id>[:<flags>]]` exactly as the burst form does. Three
**netconf** keys configure it, so the network configures it and not each
server: `sasl.server`, `sasl.mechanisms`, `sasl.timeout`. The `sasl`
capability is advertised only while a server name and a mechanism list
are set *and* a server by that name is linked (`sasl_available()`);
`/STATS S` lists the exchanges in flight. `FLAG_SASL` marks a
registration with one under way.

**Signed tokens** (`include/ircd_token.h`, `ircd/ircd_token.c`). What the
mail verification and the upload ticket are both made of, extracted
because the second one wanted it: `1.<b64(expiry:payload)>.<b64(16 bytes
of HMAC)>`, the key derived from the `Security{}` key by a **label** —
so a token minted for one purpose cannot be presented as another, and
neither can be used to cipher a hostname. Nothing is stored: every server
has the key by construction, which is what makes a token minted on one
recognisable on another, and what makes there be no table to expire.
Sixteen bytes of tag because a person pastes this back and 128 bits is
far past what forging one is worth.

**Files** (`modules/services/filehost/`, `doc/readme.files`, proposal 006
§7.4). Two routes, one command, one table and a directory, on top of the
HTTP layer. The server never receives a file over IRC: `/FILE UPLOAD`
mints a **ticket** — `<id>:<account>:<target>`, signed, fifteen minutes —
and answers with a `curl` line, which is what makes this usable from a
client written in 1998. **The identifier is in the ticket as well as in
the path** and they must match, or a ticket would be a ticket to
overwrite somebody else's file. **Only an identified client may upload**:
an account is a nickname, and filing an upload under a bare nick files it
under whoever wears that nick next week. The objects live under two
characters of fan-out and the row is written **after** the move, because
a row pointing at nothing is worse than an object nobody has a row for —
the sweep collects that. **Only a short list of content types is served
as itself** (image, audio, video, text/plain); everything else is
`application/octet-stream`, `attachment`, `nosniff`, because serving the
type the uploader declared is a way to put script on the server's own
origin. The quota is checked **before** the ticket is minted, since
telling somebody where to send a file and refusing it on arrival wastes
their upload. `file_store.c` holds every statement and everything that
touches the disk, the way `hist_store.c` does — which is also what makes
object storage one file's worth of work the day it is wanted. There is
deliberately no `draft/filehost` tag: a tag is read by clients, and the
clients are the next phase.

**Never `timer_add(timer_init(&t), …)` from inside `t`'s own callback.**
`timer_init()` zeroes the generator's flags, `GEN_MARKED` among them, and
that flag is the only thing telling `timer_add()` it is re-arming a timer
`timer_run()` still holds — without it the timer is queued twice and the
server dies later on an event for a generator that is no longer active.
Every deadline timer here re-arms from inside its own expiry (a callback
starts new work), so `cache.c`, `hooks.c` and `sasl.c` all `timer_init()`
once and `timer_add()` the same struct thereafter, the way
`check_pings()` does.

**Umode `+f`, the freeze** (`include/user_flags.h`, `ircd/parse.c`). Marks
a client the services are holding while they work out whether it may keep
the nickname it is wearing. It is a core mode only a server or a `+S` bot
may set, restored in both directions for anyone else by the same gate that
guards `+r`, shown in `WHOIS` as 692, and enforced in `parse_dispatch()` —
the one place both dispatch paths meet and past the point the parameters
are laid out. What a frozen client may still send is declared by each
command with `MFLG_FROZEN_OK` in `msgtab[]` rather than listed in
`parse.c`, so a module's command can declare it too; `PRIVMSG` and
`NOTICE` carry the flag but are narrowed to a single local `+S` target,
since what the state must allow is talking to the service that will lift
it. **An account lifts it** — both `ms_account()` and the `+r` case in
`do_user_mode()` clear it, because an account is exactly the proof `+f`
says is missing — and so does a `-f` from a server or a bot. What happens
*after* the freeze (a grace period, a rename, a KILL) is the services
node's policy and not this server's.

**History** (`modules/services/history/`, `doc/readme.history`, proposal 006
§7.1). What was said, kept, on PostgreSQL through `db.h` — phase 2. It
attaches to `HOOK_MESSAGE_DELIVERED` and writes what it hears; nothing here
blocks, and a database that is down costs the rows it did not store and
nothing else (failures are logged at most once a minute, because a dead
database fails one insert per message on the whole network). **Every server
writes what it delivers** and the database throws away the copies: there is
no designated archivist, because that is a server whose split takes the
record with it. That works only because the key of a row — `(sent_at,
msgid)`, in that order because a partitioned table will not take a unique
constraint without its partition key — travels *with* the message, which
needs `FEAT_NETWORK_FEATURES` and `FEAT_NETWORK_TIME` on; the module says so
at load if either is off. The schema is one table partitioned by month, BRIN
over the time, GIN over the text, applied with `/MODULE MIGRATION APPLY
history`. **A `DEFAULT` partition is a trap unless something moves rows out
of it**: PostgreSQL refuses to create a partition whose range the default
already holds rows for, so everything stored before the schema was migrated
blocked its own month for ever — `history_ensure_partition()` (v3) rescues
them inside the statement that creates the partition, and the maintenance
timer runs every five minutes instead of every six hours until the schema
answers. `hist_store.c` holds **every** statement the module sends — not to
leave a door open for another engine (§7.1 closes that) but because
retention, purging and deletion by account need one place to happen;
`history_forget()` ships in v1 rather than being added later, and takes the
other end's copy of a direct message with it, because a direct message is one
row and what is being deleted is the message. **`/HISTORY STATUS|PURGE|EXPORT|FORGET`** (`hist_admin.c`) is the
operator's side, behind a privilege of its own — `history_admin`,
`PRIV_HISTORY`, cleared from the global defaults because it is the
difference between an operator and somebody who can read every
conversation on the network. **`EXPORT` and `FORGET` use the same
predicate**, deliberately: what a person is handed has to be what a person
can have destroyed. The export is a file (JSON Lines, mode 0600, never over
an existing one) under `FEAT_HISTORY_EXPORT_DIR`, empty by default so a
server writes nothing until told where; it is paged by **keyset**, not
offset, so a long export never re-reads what it has written. A **direct
message is stored only when both ends have identified**: the only durable handle on a person is
the nickname they proved, and filing one under a bare nickname would show it
to whoever wears that nickname next week. Messages to a service are never
even reported to the module, and non-ACTION CTCP is dropped by
`hist_capture.c`.

**Threads and reactions** (proposal 006 §7.2) are the same tag pointing at
the same thing: `+draft/reply` says "this follows that", and a reaction is
a TAGMSG that also carries `+draft/react`. Both ride on the `msgid` the
network already agrees on, so nothing new was invented. A reaction **is
stored as a message** — kind 2, the reaction in `body`, `reply_to` naming
what it is about — which makes removing one and reading them back in order
the same operations as for anything else; every other TAGMSG (`+typing`)
is still dropped, because a row saying somebody was typing in March is not
history. The tag is the **client's claim, kept verbatim** and with no
foreign key: a message may reply to one the retention dropped. Both are
stored only if `CLIENTTAGDENY` allows them — whose default is now
`*,-draft/reply,-draft/react,-typing`, deny-all with those three named —
and replayed through `msg_tag_line_replay()`'s fourth argument, a
pre-rendered client-tag string the core relays without knowing what any of
it means.

**REDACT** (`modules/services/history/hist_redact.c`, IRCv3
`draft/message-redaction`, proposal 006 §7.2). It lives in the history
module and not in the core because **you cannot redact what nobody
stored** — a server with no history has nothing to delete and no way to
know who wrote the message. That is also what makes the policy checkable:
whether somebody may take a message back depends on who wrote it, which is
a database round trip away, so **nothing is relayed until the deletion has
happened** (a client that dropped the message from its view while the
store kept it is the one outcome worse than not supporting this). Who may:
the author within `FEAT_HISTORY_REDACT_WINDOW`, a channel op with no
window (moderating is not undoing), or `history_admin`. The target named
must be the one the message was sent to, or naming any channel you have
ops on would reach any other. It takes the **reactions** with it and
leaves the **replies**: a reply is somebody else's message. It reaches only
clients with the capability, and it is the one thing here that crosses a
link (token `RD`), because every server's clients saw the message; what
arrives from a peer is an announcement, since every server reads the same
store.

**SEARCH and EDIT** (`hist_search.c`, `hist_edit.c`, §§7.1-7.2). Neither is
an IRCv3 specification — the protocol has never had a word for either — so
both are vendored (`blacknode/search`, `blacknode/message-edit`), and both
live in the history module for REDACT's reason: **you cannot search or
edit what nobody stored**. SEARCH is what finally uses the GIN index v1 of
the schema created: `SEARCH <target|*> [from=|after=|before=|limit=]
:<words>`, answered as an ordinary batch of replayed messages, each
carrying **the target it was sent to** rather than the one searched. **What
may be searched is what may be read, asked when it is asked** — the
channels the client is on now and its own conversations, never a scope
meaning the network — and **the text is the first condition in the WHERE
clause**, because it is the only one the index can answer. It is
`websearch_to_tsquery`, not `to_tsquery`, which errors on an apostrophe or
a stray bracket: a search box that can be made to error is one that will
be. What to look for is the **last** parameter, where IRC has put the
free-form argument since PRIVMSG. EDIT changes what a message says
**without changing its identifier** — the replies and the reactions point
at it — and **only the author, ever**: an op may REDACT, because moderating
is taking something out of a room, not making it say something its author
did not. `FEAT_HISTORY_EDIT_WINDOW` is shorter than the redaction one
because a redaction leaves a hole and an edit leaves a sentence nobody can
tell was ever different. An empty edit and a TAGMSG are refused, not
treated as deletions. It crosses a link (`ED`) like REDACT; read back, the
row carries **`blacknode/edited`** — a *server* tag
(`msg_tag_line_replay_server_tag()`), because it is the store's statement
and not the message's, so `CLIENTTAGDENY` has no say in it, the way it has
none over `batch`.

**MARKREAD** (`hist_marker.c`, IRCv3 `draft/read-marker`, §7.2). What turns
a history into an inbox. **Per account, not per connection** — a person
reads on their phone and expects their laptop to know, so moving the marker
tells every one of that person's clients, and a client with no account is
refused. **It only moves forward**: two clients of the same person race
constantly, and a marker that could go back would make messages unread
again every time the slower one reported in; a backwards request is not an
error, the answer is just where the marker already was. It is a
**timestamp, not a msgid** — "everything up to here" is a point in time, and
a message arriving late from a split is behind the marker if it was *sent*
behind it. It never crosses a link: the store is shared, so the other
servers' copies of that person are told by their own server. Mentions need
no code at all — an account *is* a nickname, so mentioning `maria` is
mentioning the account whenever `+r` says she proved it.

**Rich text** (`modules/hooks/richtext.c`, `doc/readme.richtext`, proposal
006 §7.3). IRC has no content type, and the thing not to do about that is
invent a dialect of control codes. The format is negotiated
(`blacknode/richtext`), the body is bounded Markdown marked with
`+blacknode/format=markdown`, and **every client that did not negotiate it
is sent the plain-text equivalent the server generated** — that is the
whole design, not a nicety.

One message therefore goes out as **two bodies**, which the ordinary relay
cannot do. The mechanism is in the core, not the module: `HookContext`
gained `hc_alt` (the other body), `hc_alt_cap` (the capability that chooses
between them), `hc_alt_tag` (a client tag not to relay with the
alternative, because a tag saying "this body is Markdown" is a lie on the
other one — `msg_tag_suppress()`) and `hc_alt_set`. A `HOOK_MESSAGE_PRE_*`
hook fills them in and `ircd_relay.c` does the fan-out, the S2S copy (the
**rich** body — the next server has the same choice to make), the echo and
the delivered hook. Any module that gives a message a content type gets
this without asking. Sanitising is the **server's** (no `<`/`>` at all, no
IRC formatting codes, nesting ≤ 3, links http(s) and ≤ 256 chars, nothing
that renders empty), because a client that enforced it could be replaced by
one that did not. **What is stored is the plain text**: a transcript is
read back by whoever reads it, `CHATHISTORY` has one body per message, and
the formatting is lost to history — a real limitation and the right side to
err on.

**Channel modes.** Bits of a `chanmode_t` mask in `chptr->mode.mode`
(`include/chan_flags.h`), registered in a run-time list in `ircd/chan_modes.c`
(`channel_chan_modes()` and friends) so modules can add their own; test them with
`HasCFlag()`. The bit is not handed out — it follows from the letter (`A`-`Z` are
bits 0-25, `a`-`z` bits 26-51), the same convention `user_flags.h` follows, so
every server agrees on it without negotiating. The twelve bits above the alphabet
are `CHANMODE_RESERVED`: `MODE_ADD`, `MODE_DEL` and the `ModeBuf` bookkeeping.
Everything that renders or parses a mode walks the register; there is no table of
letters anywhere else.

**Core state.** `include/client.h` (`struct Client`, with `Connection`/`User`/`Server`
sub-structs), `ircd/channel.c` (the largest file: channels, modes, bans, ops),
`ircd/hash.c`, `ircd/whowas.c`, `ircd/numnicks.c` (P10 numeric nick encoding).
`ircd/send.c` is the outbound path; `ircd/msgq.c`/`ircd/dbuf.c` the queues.

**Configuration.** `ircd/ircd_parser.y` + `ircd/ircd_lexer.c` parse `ircd.conf`
into the structures in `ircd/s_conf.c`. The lexer expands `${NAME}` references
from the process environment (`include/ircd_env.h`, `ircd/ircd_env.c`,
`doc/readme.env`, proposal 004) inside quoted strings and, as a number, where
the grammar wants one; a missing variable is a config error, not an empty
string. The same file has the typed accessors (`env_str()`, `env_int()`,
`env_bool()`) for code that reads one variable directly. Runtime-settable knobs are "features"
(`ircd/ircd_features.c`, `doc/readme.features`), reachable via `/GET` and `/SET`.
Access control lives in `gline.c`, `sline.c`, `jupe.c`, `crule.c`, `IPcheck.c`.

**TLS.** One backend compiled in, chosen by `IRCU_TLS`: `ircd/tls_{openssl,gnutls,libtls,none}.c`
behind `include/ircd_tls.h`.

**Modules** (see `doc/readme.modules`, `include/module.h`, `ircd/module.c`). A module
is a `.so` exporting exactly one symbol, `struct ModuleInfo ircu_module`, whose
first field is `IRCU_MODULE_ABI` — compared exactly, with no backward compatibility;
the ABI changes mean recompiling modules.  A module registers commands
(`module_add_command()`), hooks (`module_add_hook()`), user modes
(`module_add_user_mode()`, which returns a server-assigned bit) and channel modes
(`module_add_chan_mode()`, whose bit follows from the letter); everything it
registers is reverted on unload. Modules run in-process with no sandbox.
They are built by the same CMake run via `ircu_add_modules()` (`cmake/IrcuModules.cmake`)
and link against nothing: symbols resolve against the ircd executable, which is
built with `ENABLE_EXPORTS`. A module that ships SQL
migrations keeps them in `migrations/` under its own directory, compiled in
rather than installed (see **Migrations**). A module that needs a library of its
own declares it in its own fragment — `modules/<type>/<name>/module.cmake`, or
`modules/<type>/<name>.cmake` for a single-file module — which may set
`IRCU_MODULE_LINK_LIBRARIES`, `IRCU_MODULE_INCLUDE_DIRECTORIES`,
`IRCU_MODULE_COMPILE_{DEFINITIONS,OPTIONS}`, or `IRCU_MODULE_SKIP` to opt out
when a dependency is missing; the core's build files never learn about it. Sources live in `modules/<type>/` (`commands`,
`modes`, `hooks`, `workers`, `services`; a type is just a directory) as either
`<name>.c` or a `<name>/` directory whose `*.c` are compiled in, `*.h` are
private and everything else is a resource copied beside the `.so`; the build
and install trees mirror that layout (`<type>/<name>.so` or
`<type>/<name>/<name>.so`). A module is identified by name, never by path or
type: `IRCU_MPATH` (default `$DPATH/modules`; macro `MOD_PATH` in config.h —
`MPATH` was already taken by the MOTD feature) is both where the build installs
and where the loader looks, searching every type directory for the two shapes
and refusing a name found under two. `Module { name = "nocaps"; };` and
`/MODULE LOAD|UNLOAD|RELOAD nocaps` (privilege `module_admin`) all take that name;
`/MODULE LIST` and `/STATS M` report state with the path relative to the module
directory (`modules/hooks/nocaps.so`), never the absolute one. A module reaches
its resources through `module_dir()`.

**The module set is the network's** (`include/module_sync.h`,
`ircd/module_sync.c`, `doc/readme.modules`). A module changes what the
server does with what arrives on a link, so **every server runs exactly
the same modules for the whole time it is on the network** — and the
difference, when there is one, shows up as a desync rather than an error,
which is why it is enforced rather than documented. Two halves. **A link
whose sets differ is refused**: `modsync_announce()` sends the digest
(`module_set_digest()`, SHA-256 over the modules sorted by name, each with
its *version* — two versions of a module are two modules; isolation is
not in it, being about failure and not behaviour) as the first thing after
the SERVER line, each side compares and each drops the link by itself, and
a peer that never says is refused at its `END_OF_BURST` because a set that
could not be compared is not one that matched. **Loading is a transaction
over every server**: `/MODULE LOAD|UNLOAD|RELOAD` makes the operator's
server a coordinator, everybody prepares, and **one failure anywhere
abandons it everywhere** — `FEAT_MODULE_SYNC_TIMEOUT`, and a server that
splits before answering counts as a failure, since it would come back
without the change. Preparing a load *is* the load (nothing short of
`dlopen()` and `mi_init` can say whether it will work), so an abort
unloads; an unload does nothing until the commit, because it cannot be
taken back; a reload converges on the module being **gone** everywhere
when one server fails, since half a network running it is worse than none.
One transaction at a time, network-wide: a server asked while busy refuses,
which aborts the newcomer. The `Module{}` blocks are the server's own file
and a rehash is **not** propagated — it is *detected*, by re-announcing the
digest and dropping the links that no longer match. An announcement that
arrives mid-transaction is ignored, not acted on: that is exactly the
window in which two servers legitimately disagree. `FEAT_MODULE_SYNC`
turns it off for one server and not for its peers, who still refuse the
link.

**Reading the config is not the same as acting on it.** The parser only
*records* `Module{}` blocks (`conf_add_module_node()`); `module_load_configured()`
acts on them, at start-up and at the end of every rehash, before
`module_sweep()` — and `module_sweep()` does not run at all after a parse
error, because the blocks past the error were never seen and every one of
them would look like a module the operator had dropped.

**Isolated modules** (`include/modhost.h`, `ircd/modhost.c`,
`ircd/modhost/`, `doc/readme.isolation`, proposal 006 §7.7). `Module {
name = "x"; isolation = "process"; }` runs a module in a host process of
its own — `ircu-modhost`, fork+**exec** so it carries neither the
server's memory nor its descriptors — with the module API travelling over
a socketpair. It gets a `struct ModuleHandle` like any other, which is
what makes `/MODULE LIST`, the rehash reconciliation and reverting its
registrations work unchanged; what the handle has instead of a `dlopen()`
handle is a process. **`isolation` is about failure, not privilege**: an
isolated module still gets everything it asks for, it just cannot reach
past the protocol.

**The asymmetry is the design: the host may block, the server may not.**
Host→server is a *synchronous* request (`feature_int()`, `FindUser()`)
answered from state the server already has; server→host is never waited
on. A veto therefore uses `HOOK_PENDING`/`hook_resume()` — which is
precisely why §5.5 had to come first — and `FEAT_HOOK_TIMEOUT` refuses
what goes unanswered. **Silence is measured as well as each deadline**: a
module that stopped answering refuses *every* registration one deadline
at a time and no operator can get in to unload it, so a host asked
something and silent for twice `FEAT_HOOK_TIMEOUT` (min 30s) is reaped.
The handshake is **synchronous with a deadline** — `module_load()`
already blocks for `dlopen()` and `mi_init`, so `/MODULE LOAD` still
answers with a loaded module or a reason.

**What crosses is bytes and a numnick, never a pointer.** A handler over
there gets a real `struct Client` built from the blob (so `cli_name()`,
`IsAnOper()`, `MyConnect()` work) that is a **copy**, freed when the call
returns. The frame is length-prefixed, big-endian, with each argument
counted *and* NUL-terminated by the encoder — so **the decoder never
writes**, which on a wire fed by the untrusted side is worth the byte an
argument. `modhost_frame.c` is that parser, tested alone (`modhost_t`);
a frame it cannot read drops the host, because there is no resynchronising
a length-prefixed stream. `modhost_decode()` does not copy, so **a frame
is valid only until the next read** — both ends consume on the following
call, and getting that wrong looks exactly like a module failing to
register.

**The API over there is a profile, not all of it** (`modhost_api.c`):
commands, hooks, the log, the allocator, the string helpers,
`ircd_snprintf()`, the features, finding a client and sending to one.
Everything else — modes, caps, bots, db, cache, HTTP, workers,
migrations, i18n — is absent, and a module that calls one **does not
load**: `RTLD_NOW`, and the error names the symbol. A silent stub and a
module believing it registered a user mode nobody has would be worse.
**The module is compiled once**: nothing in its source says which side it
runs on. `modules/commands/isolated_demo.c` is the reference and will
crash or spin on request. `enum Feature`, `enum HookType` and
`enum HandlerType` travel as numbers, which is safe only because the
handshake compares `IRCU_MODULE_ABI` between server and host.

**Workers** (`include/worker.h`, `ircd/worker.c`, `doc/readme.workers`). Optional
threads for work that would otherwise stall the core: `worker_submit()` hands a
`struct WorkTask` to a pool, `worker_spawn()` starts a thread with a loop of its
own, and both deliver results back to the main thread through a self-pipe the
event engine watches like any other descriptor — no engine was changed. The one
rule is absolute: **a worker thread never touches core state** — no `struct
Client`, no `CurrentTime`, no `MyMalloc()`, no `log_write()`, no `sendto_*`; it
gets its input copied into the task and returns its output the same way, and a
client is referred to by numnick (`worker_task_set_client()`), never by pointer.
`FEAT_WORKER_THREADS` is 0 by default, and at 0 nothing is created at all.
Modules reach this through `module_submit_work()`/`module_spawn_worker()`;
unloading a module cancels its queued work and waits for what is running, which
blocks the server. Run `ctest --preset tsan -R worker_t` after touching
`worker.c`.

**Database** (`include/db.h`, `ircd/db.c`, `doc/readme.database`). The core holds
the `Database{}` block and a table of queries in flight, and dispatches to one
registered *driver* — a module. `db_query()`/`db_exec()` take a `struct DbQuery`
(SQL with `$n` placeholders plus a NULL-terminated `struct DbParam**`) and call
back in the main thread with a `struct DbResult` whose rows are `json_t` and
whose errors are the closed `enum DbError`; no driver type ever escapes. The API
lives in the core because modules are `RTLD_LOCAL` and could not resolve each
other's symbols, and because holding the callbacks there is what lets a module
be unloaded with queries outstanding. `modules/workers/postgres/` is the libpq
driver: one dedicated worker per pooled connection, always `PQsendPrepare` +
`PQsendQueryPrepared` (never `PQexec`), with a hard deadline capped at
`DB_TIMEOUT_MAX_MS` (5s) enforced by `poll()` rather than by libpq.

**HTTP** (`include/http.h`, `ircd/http.c`, `ircd/http_server.c`,
`ircd/mongoose/`, `doc/readme.http`, proposal 006 §7.4). **The core serves
it and modules claim routes on it** — not the `db.h` arrangement, because
HTTP is not a thing a server either has or has not: a listener is one of
the server's own ports, it has to come up before any module loads and stay
up across a rehash that unloads one, and its TLS is configuration. What is
pluggable is the routes. The server is **Mongoose**, vendored in
`ircd/mongoose/` as upstream's amalgamation and **never edited** — a local
patch would give away the only reason to use somebody else's twenty
years of fuzzing on HTTP framing, and it is what brought chunked bodies,
TLS, and WebSockets and SSE for when they are wanted. It is GPL-2.0-only,
so an ircd built with it is GPL-2.0 rather than "2 or later".
`ircd/http_server.c` is the only file that knows Mongoose exists;
`ircd/http.c` is the routes, the requests in flight and the deadline, with
**one indirection left** (`struct HttpTransport`, core-internal) purely so
that all of it stays testable with no socket — `http_t`, the split
`migration.c` has from `migration_run.c`.

Routing is exact match, or a path ending in `/` matching everything under
it, longest prefix winning — deliberately the whole language. A handler
runs in the main thread and **does not have to answer there**: it keeps
the `http_req_t` and calls `http_respond()` later, under
`FEAT_HTTP_TIMEOUT`, which answers 504 for it; answering after that is
ignored, not fatal. `HTTP_BODY_MAX` bounds what crosses into the event
loop, not what HTTP can transfer.

**A body bigger than that never reaches the main thread, either way.**
Going up, the worker takes the request at `MG_EV_HTTP_HDRS` — the one
moment a body can be sent anywhere but into memory — and streams it to
`FEAT_HTTP_SPOOL_DIR`; the handler gets `hreq_file`/`hreq_filelen` and
takes the bytes with **`http_request_save()`**, which moves them wherever
they arrived (a `link()`+`unlink()` for a spooled one, a write for one
small enough to have come through memory) so **no handler asks which of
the two happened**; `http_request_bodylen()` is the length either way.
The spool file is the core's and is deleted once the request is answered.
Going down, `http_response_file()` names a file and the worker streams it,
carrying `Range` and `If-None-Match` from the request it answers. A
request refused at the headers (411 chunked, 413 over
`FEAT_HTTP_UPLOAD_MAX`, 400 for a second on one connection) sets
`is_resp`/`is_draining`, because the body is still coming and a server
that answered and then read it would answer it twice. Mongoose's upload
helper takes the connection's handlers over and does not give them back:
`httpd_upload_finished()` restores them, or the `MG_EV_WAKEUP` carrying
the answer reaches a handler with nothing left to do — which looks exactly
like a request nobody answers.

**`http_available()` is `FEAT_HTTP_PORT`, not the socket** — a consumer is
asking whether its route will ever be reached, and a listener that is down
for the length of a rehash is not a module's business. **Ask it from
`HOOK_CONFIG_LOADED`, never from `mi_init`**, which runs mid-parse where
the features are not final; that is the one mistake this API makes easy
and it is silent. Claiming a route while nothing is listening is fine and
expected.

**The listener follows the configuration.** `FEAT_HTTP_PORT` is 0 by
default and at 0 nothing listens; `http_server_reconfigure()` runs once
the file has been read in full (`ircd.c` at start-up, `s_conf.c` after
each rehash, both *before* `HOOK_CONFIG_LOADED` so a module sees the same
answer it will keep), and a rehash that changed none of port, bind,
`FEAT_HTTP_MAX_CLIENTS`, `FEAT_HTTP_TLS_CERT` or `FEAT_HTTP_TLS_KEY` does
nothing, because restarting a listener drops every connection on it. TLS
here is **Mongoose's, not the ircd's** — `IRCU_TLS` picks what the *IRC*
ports speak and can be gnutls or none — so it reads its own PEM files; the
build gives Mongoose OpenSSL when the ircd already links it and Mongoose's
own otherwise — which is **TLS 1.3 and ECDSA only**, so the same PEM files
that work on an `openssl` build are refused at the handshake on a `gnutls`
or `none` one, and from the client that looks like the connection being
dropped.

**The socket is the worker's and the routes are the main thread's.** A
request goes up through `worker_post()`; an answer comes back down through
`mg_wakeup()`, which Mongoose documents as safe from any thread — so there
is no queue and no self-pipe here, the library already has one. What
crosses is a struct of **bytes** and a connection id, never a pointer.
Mongoose's own logging is compiled out (`MG_ENABLE_LOG=0`) because a
worker may not touch the core's log; a failed listen comes back as the
task the worker posts. **The order in `http_server_stop()` is the whole of
the thread safety**: clear the transport (so no further `mg_wakeup()` can
be issued), then `worker_stop()`, which joins the thread, which frees the
manager last.

**What Mongoose does not decide is what a path means**, and that is
`http_server.c`'s: `..`, `%2F`, `%5C`, `%00` or a control byte is 400,
because the path is handed to modules and one of them will open a file.
The path a handler gets is decoded; the query string is not.

**`server_die()` stops the threads before it closes the descriptors.**
`close_connections()` closes every descriptor there is, by number, without
knowing whose it is; a worker in `poll()` on its own stop pipe would have
that number freed under it and handed back to the next open. Everything
after `worker_shutdown()` there is single-threaded again, and the modules
are still unloaded where they always were, after the event loop, with
their workers already gone — which the worker API allows for.

**Cache** (`include/cache.h`, `ircd/cache.c`, `doc/readme.cache`). A
key-value store in front of the database, with the `Database{}`
arrangement exactly: the core holds the `Redis{}` block and the calls in
flight and dispatches to one registered driver — a module —
`modules/workers/redis/` being it (hiredis, one dedicated worker per pooled
connection, synchronous on purpose because an event-driven client would have
to be woven into the server's own loop to avoid what the workers already
avoid). **The cache is never the truth**: everything that reads it reads the
database too, so a cache that is missing, empty, stale or down costs a query
and nothing else — which is why `cache_get()` returning 0 (no driver, no
block) needs no branch of its own, being the same thing as a miss. The rules
that make it worth having: **cache the misses too** (the majority answer is
"not registered", and a cache of hits only leaves most traffic reaching the
database anyway), and **invalidate on write** rather than waiting for the TTL
(the store is shared, so one `cache_del()` clears it for every server). The
prefix is applied by the core, once, so a driver cannot forget it and two
networks sharing a store cannot read each other's keys. Values are opaque
bytes, binary-safe, and carry JSON in practice. A call has its own deadline
(clamped to `CACHE_TIMEOUT_MAX_MS`, 2s — slower than that is not a cache) on a
timer of `cache.c`'s own, is dropped when its module unloads, and is failed
when the driver is withdrawn still owing it. `cache_t` covers the register
without a store.

**Migrations** (`include/migration.h`, `ircd/migration.c` + `ircd/migration_run.c`,
`doc/readme.migrations`). A module with SQL migrations is a *directory* module
with a `migrations/` subdirectory holding `v<N>_<name>.{up,down}.sql`. The build
compiles them into the `.so` as string literals (`cmake/IrcuMigrations.cmake`) —
they are never copied beside it — and the loader `dlsym`s them, validates them
(format, `[A-Za-z0-9_]` names, versions 1..N without gaps, an up for every down)
and **refuses the load** with a message naming the bad file. `migration.c` is the
validator (no database, no client — unit-tested by `migration_t`);
`migration_run.c` is the runner. A module's migrations never run automatically:
an operator drives them with `/MODULE MIGRATION LIST|STATUS|APPLY|REVERT`. The
one exception is the core set (`ircd/migrations/`), which creates the
`migrations` table at start-up under `module_name = "core"` — a name no module
may take. Each migration is one transaction (script + its `migrations` row),
runs on its own connection off the pool, and gets `migration_timeout` rather
than the 5s query cap.

**A bot a module creates exists on every server** (`doc/readme.services`).
Every server runs the same module set, so a module that introduces
NickServ introduces it once per server and the copies collide, for ever.
There is no mechanism in the core that hides one: `bot_create()` makes a
real client, bursted like any other. A module that wants one bot per
*network* has to arrange that itself — introduce it only where a
configured name matches this server, or only on the server the operator
ran `/BOT` on. The `irc_services` module, which used a non-propagating
local bot per server with its `Service{}` blocks published over netconf,
is gone: the network's services are a separate program on a link of
their own (`doc/readme.accounting`).

**Bots and services** (`include/bot.h`, `ircd/bot.c`, `doc/readme.services`).
A bot is a `struct Client` the server introduces on its own behalf
(`make_client(&me, ...)`, no connection); `bot_create()` takes the owning
module, and unloading a module destroys its bots. `BOT_SERVICE` makes a
*service bot*: user modes `+S` (`IsServiceBot()`, `IsLocalServiceBot()`),
`+k`, `+o` and `+B`; `+B` and `+S` are core modes only a server may set —
`set_user_mode()` undoes both directions for any local client, opers included.
A service bot may change any non-oper's modes through `bot_set_user_mode()`
(what a user may set on itself; `+r` reaches the gate in `do_user_mode()`
only from a bot on the far side of a link, since that one asks for a
server source). The relay layer (`ircd/ircd_relay.c`)
never delivers to a local service bot; it calls `bot_deliver_private()` /
`bot_deliver_channel()`, which run `HOOK_MESSAGE_RECEIVED` (sender local or
remote, PRIVMSG or NOTICE, `hc_notice` says which; channel messages once per
service bot on the channel). `modules/commands/m_bot/` is the `/BOT` front
end and the reference for the API. There is no `Service{}` block any more:
the network's services are a separate program, so a bot here is whatever a
module wants one for, and a module that reads configuration lists does it
from `HOOK_CONFIG_LOADED` (fires after start-up and after each rehash —
`mi_init`/`mi_rehash` run mid-parse and must not read config lists).

**Translations** (`include/ircd_i18n.h`, `ircd/ircd_i18n.c`, `ircd/ircd_po.c`,
`ircd/m_language.c`, `doc/readme.translations`, proposal 005). Plain GNU PO
files the server reads itself — no msgfmt, no libintl — one `<code>.po` per
language under a *domain*: `core` from `PO_PATH` (`IRCU_POPATH`, default
`$DPATH/po`, sources in `po/`) and one per directory module with a `po/`
resource dir (`module_i18n(mod)`; opened before `mi_init`, closed after
`mi_fini`). `send_reply()` translates every numeric (context = the numeric's
code, so `s_err.c` is written with `N(sym, "401", fmt)`) and every
`SND_EXPLICIT` format; other text to one client is marked `_(to, s)` /
`_n(to, s, p, n)`, `N_(s)` marks for later. Never translate the log,
anything rendered once for many recipients, or anything that crosses P10 as
text. The loader rejects any entry whose `ircd_snprintf` directives differ
from the original's (order and bytes), or that has a line break, and keeps
the previous catalog for a file that does not parse. A client's preference
(`LANGUAGE`, IRCv3 `draft/languages`, numerics 687/690/981/982, up to
`I18N_PREF_MAX` codes, before or after registration) is a 16-bit index into
an interned table in `cli_lang()`; lookup is client codes (exact, then
primary subtag) → `FEAT_DEFAULT_LANGUAGE` → original, and `en` stops the
chain. It travels as the `LG` P10 token (on change, after the `N` at
registration, and in the burst). `/REHASH` reloads every domain, `ircd -k`
fails on a bad catalog, `/STATS n` lists what is loaded. `ircd_i18n_t` loads
every `.po` in the tree and fails on any rejected entry; the `pot` target
regenerates `po/core.pot` and each module's `po/<name>.pot` with xgettext.
Nothing here may run on a worker thread.

**Client capabilities** (`include/capab.h`, `ircd/capab.c`, `ircd/m_cap.c`).
IRCv3 CAP names a client negotiates. A capability is a bit position in the two
`capset_t` bitsets every connection carries — `cli_capab()` (asked for) and
`cli_active()` (in force), both `CAP_MAX` bits wide — tested with `CapHas()` /
`CapActive()`. `ircd/capab.c` is the run-time register (`cap_first()`,
`cap_find()`, `cap_register()`), the same shape as the user- and channel-mode
registers, so a module adds its own with `module_add_cap()`; `ircd/m_cap.c` is
only the protocol on top (CAP LS/REQ/ACK/LIST, and `cap_new()`/`cap_del()`,
which is why the register can be unit-tested without a client — `capab_t`).
The core's own come from `CAPLIST` and take the positions `enum Capab` names
before any module can ask; a module's position is handed out, does *not*
follow from the name (capabilities never cross a server link, so nothing has
to agree on it) and must be kept, not recomputed. Registering on a running
server sends `CAP NEW`; unregistering, or unloading the module, sends `CAP DEL`
and clears the bit from every local client. The `require`/`forbid` arguments of
the `sendcmdto_*_capab_*()` calls are positions, with `CAP_NONE` for "no
requirement" — never `0`, which is a valid position.

**Message identifiers** (`include/msgid.h`, `ircd/msgid.c`, `include/msg_tag.h`).
The IRCv3 `msgid` tag: one name for one message, network-wide, and the thing
everything that refers back to a message will be built on (threads, reactions,
edits, read markers, a history store). `msgid.c` is only the generator — the
server's P10 numeric plus a base-62 counter seeded from the clock, so a restart
never reissues one — and knows nothing about clients, which is why `msgid_t`
can test the one property that matters without the server. `msgid_init()` runs
after `init_server_identity()`, because the numeric is what separates this
server's identifiers from every other's. The identifier belongs to the *line*,
not to a send call: `parse_dispatch()` brackets the handler with
`msg_tag_line_begin()`/`msg_tag_line_end()`, so the channel fan-out, the echo
to the sender and the copy crossing every link all carry the same one.
`msg_tag_line_msgid(tok)` hands it out only when `tok` is the line's own
command, so a numeric sent while handling a PRIVMSG does not inherit the
message's name. Only PRIVMSG, NOTICE and TAGMSG get one
(`msg_tag_needs_msgid()`); WALLCHOPS/WALLVOICES are excluded because they are
echoed to their sender as a NOTICE and would get two different names. A
client's own `msgid` is never trusted (it could point at someone else's
message); one from a server is kept and forwarded, which is what makes the
whole network agree. It reaches a client only with `message-tags`, so a
traditional client sees byte-identical lines to before, and S2S only under
`FEAT_NETWORK_FEATURES`.

**Batches and labeled responses** (`include/batch.h`, `ircd/batch.c`). IRCv3
`batch` (these messages belong together) and `labeled-response` (a client names
its request with `@label=`, the server names the answer). The design point is
that **nothing is buffered**: the batch opens *lazily*, on the first message the
command actually sends (`label_before_send()`, called from `send_buffer()`), so
the server never has to know in advance how many replies there will be — if none
arrives, `label_end()` sends the bare `ACK` instead. `parse_dispatch()` brackets
the handler with `label_begin()`/`label_end()`, the same window as the msgid.
Only one label is in flight at a time — a command is dispatched, handled and
finished before the next line is read — so the state is a single context, not a
table. `batch_current()`/`batch_label_tag()` are what `msg_tag_format()` renders
as `@batch=`/`@label=`; the label rides on the opening `BATCH +id` line and on
the `ACK`, never on the messages inside nor on the closing line. A client needs
*both* `batch` and `labeled-response` or the label is ignored entirely (the spec
builds one on the other, and half of it is unreadable). `batch_client_exiting()`
clears the state for a connection that dies mid-response.
`batch_out_open()`/`batch_out_close()` are the third kind: a server telling
one client that the next several messages are one answer (what `CHATHISTORY`
needs). One is open at a time, for the same reason the label is a single
context, and while its own `BATCH` line goes out `batch_current()` answers
with whatever was already in force — so it nests inside a labeled response
the way the spec says. Note `MSG_IRCBATCH`
in `msg.h`: glibc's `<bits/socket.h>` already has an `MSG_BATCH`, so the macro
is spelled differently while the wire command stays `BATCH`. `label` is the one
non-`+` tag a client may send — `msg_tag_client_may_send()` in `msg_tag.c` — and
it goes no further than the command it arrived on.

**Multiline** (`include/batch.h`, `ircd/multiline.c`, `ircd/m_batch.c`). IRCv3
`draft/multiline`: a message longer than 512 bytes, sent as a client batch of
PRIVMSG/NOTICE pieces carrying `@batch=`, held until `BATCH -` and then relayed.
It lives apart from `batch.c` for the reason `migration_run.c` lives apart from
`migration.c` — this half needs channels, the hash tables and the relay, and
keeping them out is what lets the labeled-response logic stay unit-testable.
**The pieces go out through the ordinary relay, one at a time**, which is the
whole compatibility story: a traditional client sees the separate messages it
always saw, a `draft/multiline` client sees the same series inside a fan-out
batch (`multiline_batch_for()`, which `batch_current()` consults). A line is
**re-split on the way out** (`multiline_line_budget()`/`multiline_cut()`):
`draft/multiline-concat` joins the pieces into one line that does not fit on
the wire, and a relay that sent it as one PRIVMSG had the send layer drop the
tail without saying so. Every piece after the first carries the concat tag
back out — only to a client that negotiated the capability
(`multiline_concat_for()`, which `msg_tag_format()` consults the way it
consults `batch_current()`, with a profile bit of its own so the prefix cache
cannot hand one recipient's bytes to another) — so it joins the line back
exactly, while everybody else still sees separate messages. The tag is the
server's own statement about its framing, like `batch`, so `CLIENTTAGDENY`
has no say in it. The whole
message carries **one** `msgid`, on the `BATCH +` line
(`msg_tag_line_force_msgid()`; NULL disarms it so the closing line, a mere
delimiter, carries none) and the pieces carry none. `draft/multiline-concat`
appends to the previous piece instead of starting a line. Limits are
`FEAT_MULTILINE_MAX_BYTES`/`_MAX_LINES`, advertised in the capability value by
`batch_multiline_advertise()` (re-run on rehash). **A piece is charged bytes
but not the flat per-command flood penalty** (`multiline_in_progress()` in
`parse.c`): at 2s per line a client sending the 24 the spec allows would be
throttled off the server for sending one message. The **Excess Flood ceiling**
is raised to match for a client that negotiated the capability
(`multiline_flood_ceiling()`, called from `read_packet()` *after* the class has
decided the throttle exemption, so it does not hand that out too): the recvQ is
measured before anything is parsed, so a client sending the `max-bytes` it was
promised would otherwise be killed before the first piece was read, and an
advertised limit you are killed for using is a trap. `batch` and
`draft/multiline-concat` join `label` as tags a client may send
(`msg_tag_client_may_send()`), and `msg_tag_format_s2s()` drops `batch`
explicitly — a batch is between one server and one client, and long messages
cross P10 as the separate messages they are made of.

**The protocol SDK** (`sdk/`, `doc/readme.sdk`, proposal 006 §7.6).
TypeScript, in this repository because the wire is one thing: a change to
`msg_tag.c` and a change to `sdk/packages/protocol` are the same change.
Two packages, split the way `sasl.c` is split from `m_authenticate.c` —
`@blacknode/irc-protocol` is the wire and nothing else (no sockets, no
timers, no globals, no dependencies, so it can be tested exhaustively)
and `@blacknode/irc-client` is a connection, the state it carries and the
conversation on top. **What it needs from a runtime is declared in one
file** (`packages/protocol/src/globals.d.ts`: `TextEncoder`,
`TextDecoder`, `setTimeout`, `clearTimeout`) and the tsconfig has no DOM
library, which is the portability contract that makes web (Next.js on
Bun), React Native and Wails the same code — and why base64 is twenty
lines of our own rather than `btoa` or `Buffer`. **A WebSocket frame is a
message, not a stream**: `text.ircv3.net` is one IRC message per frame
with no CR LF, so the transport holds nothing between frames. An account
**is** a nickname, so `User` has `identified` and `frozen` and there is no
`account` field to keep beside the nick. The browser connection lives in
a `WorkerHub` (many tabs, one connection; a snapshot and not a replay for
a tab that just opened; one notification, in one tab, preferring a hidden
one) which knows nothing about `self` and is therefore tested with a fake
port. `bun test` needs no server; `packages/client/test/live.ts` needs
one and is not part of it — it is what found both halves of the multiline
bug, which no fake transport could have.

**SASL** (`include/sasl.h`, `ircd/sasl.c`). The *shape* of an exchange and
the routing, never the answer: the AUTHENTICATE lines become an `XQUERY`
to the services server and the `XREPLY` comes back through
`m_xreply.c`'s `sasl:` prefix. See **SASL is relayed, never answered**
above. A session holds a password, so every way out wipes it rather than
just freeing it.

**Base64** (`include/ircd_base64.h`, `ircd/ircd_base64.c`). RFC 4648, the
standard alphabet — not the P10 one in `numnicks.h`, which avoids `+` and `/`
because its output goes in a nick. The decoder is strict (length a multiple of
four, padding only at the end, nothing outside the alphabet) because it decodes
what a client sent; a lenient decoder is how one message gets two encodings.
Both directions are covered by the RFC 4648 §10 vectors in `crypto_t`.

**Cryptography** (`include/ircd_sha256.h`, `ircd_aes.h`, `ircd_argon2.h`,
`ircd_pwhash.h`). The server's own, never the TLS backend's: `IRCU_TLS` can be
`none` and can be GnuTLS or libtls, so reaching for whichever library happens to
be linked would work on one build and not compile on the next. SHA-256 +
HMAC-SHA-256 (signing what the server hands out and must recognise again),
AES-256 **in GCM only** (there is deliberately no unauthenticated-block
interface — a cipher without a tag is one whose output anybody can edit),
BLAKE2b, and Argon2id for passwords. `ircd_pwhash.c` is the storable form,
`$argon2id$v=19$m=…,t=…,p=…$salt$tag`: **the costs travel with the hash** and
are not read from the config when verifying, which is what lets them be raised
without invalidating every stored password (`ircd_pwhash_outdated()` says when
to re-hash). It takes an optional server-wide *pepper* so a stolen database
alone is not enough to start guessing. Every constant that could be mistyped is
either computed (the AES S-box, from its GF(2^8) definition) or covered by a
published test vector — FIPS 180-4, RFC 4231, FIPS 197 C.3, the GCM test cases,
RFC 7693 and RFC 9106 — in `crypto_t`; that is the only thing that can tell a
wrong digit from a right one, since either way it compiles and runs. **Never
hash a password on the main thread**: Argon2 takes 50–250 ms and tens of
megabytes *on purpose*, so ten simultaneous logins would stop the server for a
second. `ircd_pwhash_make()`/`_verify()` are pure — no core state, nothing that
outlives the call — precisely so they can go through `worker_submit()`. There
is no bcrypt: it is 1042 constants that would have to be transcribed, its only
use here would be importing hashes from a system that does not exist yet, and
when it is wanted the right move is to vendor Openwall's `crypt_blowfish`
rather than retype the tables.

**Hooks** (`include/hooks.h`, `ircd/hooks.c`). A closed enum of lifecycle points
modules attach to. Points named `HOOK_*_PRE_*` run before the server acts and may
veto (`HOOK_DENY`) or, for messages, rewrite; the rest are after-the-fact
notifications whose return value is ignored. All hooks run inline on the main thread.
`HOOK_COMMAND_PRE`/`HOOK_COMMAND_POST` are the exception in shape: they fire
around *every* command, from the two dispatch sites in `ircd/parse.c`
(`parse_dispatch()`), so a module can watch one user act on another without a
hook per command. They are registered with `module_add_command_hook()` naming a
command (`hook_add()` refuses them), and `hooks.c` stays a pure dispatcher —
`parse.c` resolves the subject each command declares in `msgtab[]`
(`struct MsgSubject` in `msg.h`: parv indices, `0` for none, `MS_LAST` for a
target whose position moves with the parameter count, and `subject_s` when the
server form has a different shape) into `hc_client`/`hc_channel`/`hc_arg`, using
`findNUser()` on the server path and `FindClient()` on the client one. Rules: a
`+S` source is skipped unless the hook passed `HOOK_CMD_INCLUDE_SERVICES`; a
`HOOK_DENY` counts only when the source is `MyConnect()` (vetoing a command
another server already applied would desync this one); `POST` is skipped after
`CPTR_KILLED` and after a veto; `hcc_parv` is read-only; recursion is capped.
`modules/hooks/cmdaudit.c` is the reference module.

**`HOOK_MESSAGE_DELIVERED`** is the one message hook that sees the whole
network. `HOOK_MESSAGE_PRE_{CHANNEL,PRIVATE}` fire only where a message
started, so a module built on them would store what this server's own users
typed and nothing else; this one fires from *every* relay path in
`ircd/ircd_relay.c`, local origin and link alike. It carries a fourth
pointer, `hc_message` (`struct HookMessage`): kind, the network-wide
`msgid`, the ISO 8601 time (the `time` tag it arrived with, so every server
records the same instant), the target as written, and whether the source is
remote. Every server that relays the message fires it, so **a store
deduplicates on `msgid`** rather than assuming it is told once — and that
needs `FEAT_NETWORK_FEATURES`, which is what carries `msgid` over P10. A
message **to a service** (`+S`/`+k`) is never reported, because that is
where passwords go and the owning module already gets it through
`HOOK_MESSAGE_RECEIVED`; a **masked** message is not either, having no
target to file it under.

**Suspending a hook** (`HOOK_PENDING`, `hook_run_suspendable()`,
`hook_resume()`). A hook that has to ask something slow — a database, an
Argon2 verification on a worker — holds the operation instead of answering.
It works only where the core has a way back in, and a module sees exactly
where that is: `hc_token` is non-zero there and zero everywhere else, where a
`HOOK_PENDING` is logged and read as `HOOK_CONTINUE`. Today the one such point
is `HOOK_CLIENT_PRE_REGISTER`, which is why it fires from `auth_module_check()`
in `s_auth.c` and not from `register_user()`: registration is already a state
machine that waits (ident, DNS, CAP, the PING cookie, iauth), so a module hold
is one more flag beside them (`AR_MODULE_PENDING`/`AR_MODULE_CHECKED`) — inside
`register_user()` there would be nothing to come back to. Suspending stops the
chain; the hold is dropped when the client leaves, and **refused** when
`FEAT_HOOK_TIMEOUT` passes or the module is unloaded still owing an answer
(failing open would be the outcome the veto was asked to prevent). `hooks.c`
keeps the list and arms one absolute timer over the earliest deadline — a ride
on `check_pings()` was wrong because that pass is scheduled minutes ahead on an
idle server. While a registration is held the nick is frozen (`m_nick.c` answers
437), because the question the module was asked was about that name.
`modules/hooks/slowauth.c` is the reference module.

**Design docs.** `doc/proposals/` holds the accepted designs for the module API
(001), the multithreading direction (002, in Spanish — option B is what
`ircd/worker.c` implements), the channel modes by module (003, in Spanish),
configuration from the environment (004) and translations with PO files (005,
in Spanish);
read the relevant one before changing either subsystem.  `006` (in Spanish) is
not a subsystem design but the roadmap for turning this into a unified
communications server: read it before starting anything that belongs to one of
its phases, and **read what it says about a phase against what is in the tree**
— each finished phase has been written back into it, including where the
implementation decided otherwise. Phases 0 (foundations), 2 (history),
3–4 (conversation and rich text), 5 (HTTP and files) and 7 (module
isolation, which is `modhost`) are done. What is left is 6 — web and
mobile clients, of which the SDK is the half that lives here — and §7.5,
voice and video, deferred on purpose and with no dependencies in either
direction.  `007` (in Spanish, revision 2) was that roadmap's phase 1 —
an account that *is* a nickname, an identity module inside the server,
NickServ as a module, the `guest-*` rename, mail — and it is **reverted**:
the file is kept as the record of a design that was built and then taken
back out, and nothing in it describes the tree. The accounting model is
the classic ircu one; read `doc/readme.accounting` instead.
Other useful docs:
`doc/p10.html` (protocol), `doc/readme.accounting` (the account and the
hidden host), `doc/readme.services` (bots, `+S`, `+B`),
`doc/readme.xquery` (how SASL reaches the services), `doc/readme.modules`,
`doc/readme.workers`,
`doc/readme.database`, `doc/readme.http`, `doc/readme.files`,
`doc/readme.isolation`,
`doc/readme.migrations`, `doc/readme.history`,
`doc/readme.richtext`, `doc/readme.translations`, `doc/readme.sdk`,
`doc/features.txt`, `doc/api/` (subsystem notes; `Doxyfile` at the root
generates reference docs).

## Conventions

- C99, K&R-ish 2-space style; `.indent.pro` and `doc/readme.indent` define the
  formatting. Doxygen comments (`/** ... */`) on public declarations.
- Add a new command as `ircd/m_<cmd>.c` + an `msgtab[]` entry + a prototype in
  `include/handlers.h`; add the source to `ircd/CMakeLists.txt`.
- Generated at build time, never edited: `ircd_parser.c/h` (bison), `chattr.tab.c`
  (via `table_gen`), `version.c`. `include/patchlist.h` is written by `ircd-patch`.
