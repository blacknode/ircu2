# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

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

Docker topologies (hub-only, full network, TLS, limits, DNS, standalone TLS hub,
NETWORK_FEATURES compat) share one compose project and are mutually exclusive;
`tests/conftest.py` groups tests by topology at collection time, so any selection
is safe. Markers are declared in `tests/pyproject.toml`. Regenerate the test PKI
with `tests/docker/generate-certs.sh`.

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

**Accounting** (`doc/readme.accounting`). There are no account names, ids or
flags and no `ACCOUNT` command. Umode `+r` means "identified to the nick in
use", `cli_user()->account` is that nick, and only a server, a `+S` service bot
(`bot_set_user_mode()`, never on an oper) or a NICK burst sets or clears it;
a nick change clears it on every server without anything on the wire, and the
`+r` letter takes no parameter in P10. WHOIS reports it as 307. Every user is
`+x` from `register_user()` on and cannot remove it: `hide_hostmask()` derives
the visible host from the IP with the TEA cipher in `ircd/ircd_vhost.c`
(`xxxxxx.yyyyyy.v4|v6`, unit-tested against IRC-Hispano vectors in
`vhost_t`) under the mandatory `Security { virtual_host_key = "<12 base64
chars>"; }` block, which must be identical on every server; bots (`+B`/`+S`)
keep their configured host. SASL, account-notify, account-tag, extended-join,
WHOX `%a` and the HOST_HIDING/HIDDEN_HOST features are gone.

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

**Bots and services** (`include/bot.h`, `ircd/bot.c`, `doc/readme.services`).
A bot is a `struct Client` the server introduces on its own behalf
(`make_client(&me, ...)`, no connection); `bot_create()` takes the owning
module, and unloading a module destroys its bots. `BOT_SERVICE` makes a
*service bot*: user modes `+S` (`IsServiceBot()`, `IsLocalServiceBot()`),
`+k`, `+o` and `+B`; `+B` and `+S` are core modes only a server may set —
`set_user_mode()` undoes both directions for any local client, opers included.
A service bot may change any non-oper's modes through `bot_set_user_mode()`
(what a user may set on itself, plus `+r`/`-r`). The relay layer (`ircd/ircd_relay.c`)
never delivers to a local service bot; it calls `bot_deliver_private()` /
`bot_deliver_channel()`, which run `HOOK_MESSAGE_RECEIVED` (sender local or
remote, PRIVMSG or NOTICE, `hc_notice` says which; channel messages once per
service bot on the channel). `Service{}` blocks (`struct ServiceConf`,
`conf_service_list()`) declare the bots; `modules/services/irc_services/`
creates them on `HOOK_CONFIG_LOADED` (fires after start-up and after each
rehash — `mi_init`/`mi_rehash` run mid-parse and must not read config
lists), reconciles them on rehash, and brings one back after a KILL or
collision. `modules/commands/m_bot/` is only the `/BOT` front end.

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
clears the state for a connection that dies mid-response. Note `MSG_IRCBATCH`
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
batch (`multiline_batch_for()`, which `batch_current()` consults). The whole
message carries **one** `msgid`, on the `BATCH +` line
(`msg_tag_line_force_msgid()`; NULL disarms it so the closing line, a mere
delimiter, carries none) and the pieces carry none. `draft/multiline-concat`
appends to the previous piece instead of starting a line. Limits are
`FEAT_MULTILINE_MAX_BYTES`/`_MAX_LINES`, advertised in the capability value by
`batch_multiline_advertise()` (re-run on rehash). **A piece is charged bytes
but not the flat per-command flood penalty** (`multiline_in_progress()` in
`parse.c`): at 2s per line a client sending the 24 the spec allows would be
throttled off the server for sending one message. `batch` and
`draft/multiline-concat` join `label` as tags a client may send
(`msg_tag_client_may_send()`), and `msg_tag_format_s2s()` drops `batch`
explicitly — a batch is between one server and one client, and long messages
cross P10 as the separate messages they are made of.

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

**Design docs.** `doc/proposals/` holds the accepted designs for the module API
(001), the multithreading direction (002, in Spanish — option B is what
`ircd/worker.c` implements), the channel modes by module (003, in Spanish),
configuration from the environment (004) and translations with PO files (005,
in Spanish);
read the relevant one before changing either subsystem.  `006` (in Spanish) is
not a subsystem design but the roadmap for turning this into a unified
communications server (rich text, history, voice/video/screen share): read it
before starting anything that belongs to one of its phases. Other useful docs:
`doc/p10.html` (protocol), `doc/readme.modules`, `doc/readme.workers`,
`doc/readme.database`, `doc/readme.migrations`, `doc/readme.translations`,
`doc/features.txt`, `doc/api/` (subsystem notes; `Doxyfile` at the root
generates reference docs).

## Conventions

- C99, K&R-ish 2-space style; `.indent.pro` and `doc/readme.indent` define the
  formatting. Doxygen comments (`/** ... */`) on public declarations.
- Add a new command as `ircd/m_<cmd>.c` + an `msgtab[]` entry + a prototype in
  `include/handlers.h`; add the source to `ircd/CMakeLists.txt`.
- Generated at build time, never edited: `ircd_parser.c/h` (bison), `chattr.tab.c`
  (via `table_gen`), `version.c`. `include/patchlist.h` is written by `ircd-patch`.
