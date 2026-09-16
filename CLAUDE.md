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

Docker topologies (hub-only, full network, TLS, limits, **identity**, DNS,
standalone TLS hub, NETWORK_FEATURES compat) share one compose project and are
mutually exclusive; `tests/conftest.py` groups tests by topology at collection
time, so any selection is safe. Markers are declared in `tests/pyproject.toml`.
Regenerate the test PKI with `tests/docker/generate-certs.sh`. The `identity`
topology is one ircd plus a PostgreSQL (`tests/identity_db/`); the image ships
every module under `/opt/ircu/lib/modules`, and the tests create the schema
themselves with `/MODULE MIGRATION APPLY identity` — **until they do, every
client is renamed to `guest-*`**, because a lookup against a table that does
not exist fails and a failed lookup is never "free".

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

**Accounting** (`doc/readme.accounting` for the model, `doc/readme.sasl` for
how a user earns `+r`). There are no account names, ids or flags: an account
**is** a nickname. Umode `+r` means "identified to the nick in use",
`cli_user()->account` is that nick, and only a server, a `+S` service bot
(`bot_set_user_mode()`, never on an oper), a NICK burst or the core itself
(`account_login()`, when the core is what checked the credential) sets or
clears it;
a nick change clears it on every server without anything on the wire, and the
`+r` letter takes no parameter in P10. WHOIS reports it as 307. Every user is
`+x` from `register_user()` on and cannot remove it: `hide_hostmask()` derives
the visible host from the IP with the TEA cipher in `ircd/ircd_vhost.c`
(`xxxxxx.yyyyyy.v4|v6`, unit-tested against IRC-Hispano vectors in
`vhost_t`) under the mandatory `Security { virtual_host_key = "<12 base64
chars>"; }` block, which must be identical on every server; bots (`+B`/`+S`)
keep their configured host. account-notify, account-tag, extended-join,
WHOX `%a` and the HOST_HIDING/HIDDEN_HOST features are gone and stay gone —
with an account that *is* the nick, the first three repeat the prefix. SASL
and `ACCOUNT` came back with phase 1 of the roadmap (proposal 007) and are
documented in `doc/readme.sasl`; the `AC` P10 token did not, and is left
unclaimed on purpose.

**Accounts** (`include/account.h`, `ircd/account.c` + `ircd/account_user.c`,
proposal 007). An account **is** a nickname, so `+r` keeps its literal meaning
and there is no mapping to maintain. The core does not know whether a password
is right: one module registers as the *provider* (`account_register_provider()`,
the shape of `db_register_driver()` and for the same two reasons) and answers
`ap_verify()` / `ap_lookup()` later, in the main thread, through
`account_complete()`. The file is split the way `migration.c` is split from
`migration_run.c`: `account.c` is the register, the questions in flight and the
guest-name generator and never dereferences a `struct Client` (unit-tested by
`account_t`); `account_user.c` applies an answer, which needs the hash tables,
the nick machinery and the send layer. A question has a deadline
(`FEAT_ACCOUNT_TIMEOUT`) enforced by a timer of `account.c`'s own, is dropped
when its client leaves, and is failed when the provider is unloaded still
owing it. **`ACCOUNT_NICK_UNKNOWN` is not `ACCOUNT_NICK_FREE`** — a lookup that
could not be answered must never be read as "nobody registered it". Granting
`+r` and taking the nickname are one act (`account_login()`); `account_logout()`
undoes both and renames to `guest-<8 base62>` (`FEAT_GUEST_PREFIX`), because a
client still called `maria` without `+r` is what an onlooker cannot tell from an
impostor. A guest name that is somehow taken is a KILL, not a retry: eight
random base-62 characters make it not happen, and a loop around something that
never runs is code that is never tested. `do_user_mode()`'s `+r`/`+f` gate
accepts `IsMe(sptr)` so the core can grant what it is the authority for; no
client is ever `&me` (the client path drops the prefix, and `m_mode.c` gates on
`IsServer`/`IsServiceBot` first), so nothing off a socket reaches it.

**SASL on the wire** (`ircd/m_authenticate.c`, proposal 007 §4.1). Where
`sasl.c` (the exchange), `account.c` (the question) and `account_user.c` (the
grant) meet, and the only place that holds all three. **AUTHENTICATE has no P10
token on purpose** — every server runs the identity module against the same
store, so there is nothing to route and no half-open session on the far side of
a split; what travels is `+r`, which travelled already. The `sasl` capability's
value is the mechanism list and it is **withdrawn when no provider is
registered** (`sasl_advertise()`, called from both registers, `CAP NEW`/`CAP
DEL` doing the announcing). The two ways in differ: a registered client is
simply logged in, while one authenticating *during* registration cannot be —
a client that is not a user yet cannot carry a user mode. So the nickname is
taken immediately (`account_claim_nick()`, so it is introduced to the network
as itself rather than renamed a moment later), the **900/903 are sent at once**
(a client waits for 903 before sending `CAP END`, and `CAP END` is what lets
registration finish — holding the numeric deadlocks both sides), and only the
`+r` grant waits for `sasl_registered()` at the end of `register_user()`.
`AR_SASL_PENDING` holds registration meanwhile, one more flag beside ident,
DNS, CAP and the PING cookie. **A continuation of an exchange is charged bytes
but not the flat per-command flood penalty** (`sasl_in_progress()` in
`parse.c`), the same exemption multiline pieces get: a chunked credential would
otherwise cost its length in chunks times two seconds before the client had
finished connecting. Starting an exchange *is* charged in full, and
`SASL_MAX_ATTEMPTS` caps how many times, so the exemption cannot be had free.

**ACCOUNT** (`ircd/m_account.c`, proposal 007 §4.2). `ACCOUNT LOGIN <address>
<password> [<account>]`, `ACCOUNT LOGOUT`, `ACCOUNT LIST` — SASL for every
client that does not speak IRCv3, and the same question underneath: the
credential goes to `sasl_login_request()` in `m_authenticate.c`, which is
where AUTHENTICATE's go, so there is **one** path from a credential to `+r`.
What the command decides on its own is only which numerics the client is
answered in — a client that never negotiated `sasl` is never told about SASL
(no 903, and failures come back as `ERR_ACCOUNTFAIL` 983 with the reason).
`LOGIN` works before registration too, behaving like `PASS`. **`LIST` needs
`cli_user()->email`**, which exists only because this client authenticated
with it; there is no form that lists somebody else's accounts, because that
is an enumerator whether or not the answer is filtered. `ACCOUNT` has these
three subcommands and does not grow: registering an account or changing a
password is policy, and policy is nickserv's. **Like AUTHENTICATE it has no
P10 token**, for the same reason plus one more — `AC` is the historical ircu
account burst, and reusing it would land an old peer's burst on a client
command. The provider answers a listing through `ap_list()`, which is
**required**, not optional: a provider that could verify but not list would
make `LIST` say "not available" on a server where identity works, which is
the one answer a user cannot tell from an outage. The attempt cap is three
*consecutive failures*, reset by a success, because switching account is a
login.

**The identity module** (`modules/services/identity/`, proposal 007 §9.1).
The provider behind `+r`, and only that: no bot, no commands, not one line
sent to a user. It owns the schema — `identity` (an address) and `account`
(a nickname), in `migrations/`, applied with `/MODULE MIGRATION APPLY
identity` — and answers the three questions `AccountProvider` asks. A
verification is **one query then one worker**: the row and the candidate
account come back together, the Argon2 goes to `worker_submit()` (never the
main thread — with `FEAT_WORKER_THREADS` at 0 it answers
`ACCOUNT_ERR_UNAVAILABLE` and says so, because hashing here would stop the
server for a quarter of a second per login), and nothing is revealed about
the address until the password has been checked — "no such address" and
"wrong password" are one answer. `ircd_pwhash_outdated()` re-hashes *after*
the client has been answered, guarded by the hash it replaces so a password
changed in between is not overwritten. The nickname lookup is
**cache-then-database**: `nick:<canon>` in Redis, the row in PostgreSQL,
misses cached too, and a cache that is down, empty or unparseable costs a
query and nothing else — while a *database* that is down gives
`ACCOUNT_NICK_UNKNOWN`, never `FREE`. `nick_canon` is `ToLower()`, not
`lower()`, because `[`/`]`/`\` are IRC capitals; the email is ASCII
lower-cased, because an address is not a nickname. The pepper comes from
`$IRCU_PASSWORD_PEPPER`, not the config file. `cert_fingerprint` is what
SASL EXTERNAL matches on, and matching it *is* the proof, so there is no
hash to check. **Writing goes through `ap_change()`** — one entry point and
one `struct AccountChange` for register, password and drop, `DbQuery`'s
shape and for its reason — and every write is read the row, one Argon2 hop,
one statement. That statement calls a **SQL function** created by migration
v2, not SQL this module composes: counting an address's accounts and then
inserting one is a race between two servers, `db.h` has no transactions
(a pooled connection is not the caller's to hold), and a statement sent on
its own runs in an implicit transaction — so `pg_advisory_xact_lock()`
inside a function called by one statement is held for exactly that
statement. The locks are taken address-then-nickname, always in that order,
because the only thing preventing a deadlock is that nobody writes the
other order. The unique indexes stay, as what catches a hand-written
`INSERT`. Every successful write `cache_del()`s the nickname rather than
waiting for the TTL, because the store is shared.

**The grace period** (`modules/services/irc_services/nick_policy.c`,
proposal 007 §§5–7). What happens to a local client using a registered
nickname it has not proved is its own: NickServ warns it, sets `+f`, and
renames it to `guest-*` when the grace period (`"grace_period"` in the
`Service{}` block, 60s by default) runs out. It is a grace period and not a
veto because the answer comes from a database and neither registration nor
a nick change can be held waiting for one. **It lives inside
`irc_services`, not in a module of its own** — that module already creates
the bot the `Service { type = "nickserv"; }` block declares, and a second
module creating a bot for the same block is a collision, not a layer.
Three things lift a freeze, and the first is the core's: `do_user_mode()`
clears `+f` in the same mode change that grants `+r`, so *every* path in
(SASL, `ACCOUNT`, `/msg NickServ IDENTIFY`) lifts it without having to
remember to. The other two are the deadline and a change to an
unregistered nick. **The hold is advisory and the deadline is where it is
checked**: identifying under the nickname already in use produces no nick
change to hang an event on, so the condition is re-tested where it
matters. **`ACCOUNT_NICK_UNKNOWN` is acted on, never ignored**: at
registration the client comes in as `guest-*`, at a nick change it is put
back under the name it had (which grants it nothing new) — but **none of
this runs with no provider registered**, because §7 is about a service
that failed, not a network without accounts. When the provider is unloaded
with people still frozen, `account_provider_gone()` renames them all:
unfreezing them in place would leave a possible impostor holding the name
with nobody watching. `/msg NickServ IDENTIFY` hands its credential to
`sasl_login_request()` like everything else, and defaults the account to
the nickname in use when the client is frozen, since that is the one it is
being asked to prove; `svc_dispatch()` wipes its copy of every line on
every way out, so a command that carries a password cannot forget to.
`REGISTER <address> <password>` takes the nickname **in use** (an account
is a nickname, so registering one you are not wearing is registering a name
you have not shown you can hold) and logs the client in on the spot with
`account_login()` — the password was just checked or just set, so asking
again would be ceremony; `PASSWORD <old> <new>` and `DROP <password>` need
`cli_user()->email`, and a drop ends with `account_logout()`, because a
`+r` to an account that no longer exists is not a state the model defines.
`"max_accounts"` rides in the request as `ach_max`: the limit is the
service's policy, but enforcing it has to happen inside the lock.

**Never `timer_add(timer_init(&t), …)` from inside `t`'s own callback.**
`timer_init()` zeroes the generator's flags, `GEN_MARKED` among them, and
that flag is the only thing telling `timer_add()` it is re-arming a timer
`timer_run()` still holds — without it the timer is queued twice and the
server dies later on an event for a generator that is no longer active.
Every deadline timer here re-arms from inside its own expiry (a callback
starts new work), so `account.c`, `cache.c`, `hooks.c` and `nick_policy.c`
all `timer_init()` once and `timer_add()` the same struct thereafter, the
way `check_pings()` does.

**Identity, in progress** (proposal 007). Two pieces of core state are in
place ahead of the protocol that will drive them. `cli_user()->email` is the
address a client authenticated with: **local and only local** — it never
crosses P10 (a remote user has `NULL`, which is this server saying it does not
know), `WHOIS` reports it with 691 **only to the user themselves**, not even to
an operator, and it is released together with `+r` in `do_user_mode()`, because
an identification without its address is a state the model does not define.
Set it with `user_set_email()`, never by hand. Umode **`+f` (freeze)** marks a
client carrying a registered nick it has not proved is its own: it is a core
mode only a server or a `+S` bot may set, restored in both directions for
anyone else by the same gate that guards `+r`, shown in `WHOIS` as 692, and
enforced in `parse_dispatch()` — the one place both dispatch paths meet and
past the point the parameters are laid out. What a frozen client may still send
is declared by each command with `MFLG_FROZEN_OK` in `msgtab[]` rather than
listed in `parse.c`, so a module's command can declare it too; `PRIVMSG` and
`NOTICE` carry the flag but are narrowed to a single local `+S` target, since
what the state must allow is talking to the service that will lift it.

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

**Cache** (`include/cache.h`, `ircd/cache.c`, `doc/readme.cache`, proposal 007
§3.3). A key-value store in front of the database, with the `Database{}`
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
collision. `modules/commands/m_bot/` is only the `/BOT` front end. Past the
six fields the grammar knows, a `Service{}` block takes **free-form options**
— `"max_accounts" = 3;`, a quoted name and a string or number — kept verbatim
by the core and read by the module implementing the type
(`conf_find_service_type()`, `conf_service_option()`,
`conf_service_option_int()`, which returns its default for an absent *or*
unreadable value). That is where a service's own settings live: there is no
`NickServ{}` block and no keyword per option, because a block per service
would mean a lexer keyword for everything any service ever grows.

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

**SASL** (`include/sasl.h`, `ircd/sasl.c`, proposal 007). The *shape* of an
authentication, never the answer: it turns the AUTHENTICATE lines a client
sends into a credential — an authcid (the email), an authzid (which of that
email's accounts) and a secret — and stops there. Whether the credential is
good belongs to the identity module, because the exchange is the same on every
server and the answer is not. The mechanisms are a run-time register, the same
shape as `capab.c` and the mode registers, sorted by name so the advertised
`sasl=` value does not depend on module load order; the core brings `PLAIN`
(`SASL_MECH_NEEDS_TLS` — it sends the password in the clear) and `EXTERNAL`
(the credential is the certificate fingerprint the server already has), and a
module adds its own with `module_add_sasl_mechanism()`. `sasl.c` knows nothing
about a `struct Client` or a socket — the caller copies the TLS flag and the
fingerprint into the session before starting it — which is what lets the whole
state machine be unit-tested (`sasl_t`), the same split as `migration.c`
against `migration_run.c`. The 400-character chunking, `+` and `*` live here
too. A session holds a password, so `sasl_session_clear()` wipes rather than
frees and every way out goes through it.

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
communications server (rich text, history, voice/video/screen share): read it
before starting anything that belongs to one of its phases; its phase 0 is
done.  `007` (in Spanish, revision 2) is that roadmap's phase 1, the identity model —
SASL, `ACCOUNT`, an account that *is* a nickname, the `guest-*` rename, the
freeze, Redis in front of PostgreSQL, and the split between the `identity`
module (the mechanism) and `nickserv` (the policy and the voice). `sasl.c`,
`User::email` and `+f` are its first two slices; §11 has the rest in order. Other useful docs:
`doc/p10.html` (protocol), `doc/readme.accounting` (the identity model),
`doc/readme.sasl` (how a user earns `+r`: AUTHENTICATE, `ACCOUNT`, NickServ
and the provider behind all three), `doc/readme.modules`,
`doc/readme.workers`,
`doc/readme.database`, `doc/readme.migrations`, `doc/readme.history`,
`doc/readme.translations`,
`doc/features.txt`, `doc/api/` (subsystem notes; `Doxyfile` at the root
generates reference docs).

## Conventions

- C99, K&R-ish 2-space style; `.indent.pro` and `doc/readme.indent` define the
  formatting. Doxygen comments (`/** ... */`) on public declarations.
- Add a new command as `ircd/m_<cmd>.c` + an `msgtab[]` entry + a prototype in
  `include/handlers.h`; add the source to `ircd/CMakeLists.txt`.
- Generated at build time, never edited: `ircd_parser.c/h` (bison), `chattr.tab.c`
  (via `table_gen`), `version.c`. `include/patchlist.h` is written by `ircd-patch`.
