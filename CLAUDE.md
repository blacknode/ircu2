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
built with `ENABLE_EXPORTS`. A module that needs a library of its own declares it
in its own fragment — `modules/<type>/<name>/module.cmake`, or
`modules/<type>/<name>.cmake` for a single-file module — which may set
`IRCU_MODULE_LINK_LIBRARIES`, `IRCU_MODULE_INCLUDE_DIRECTORIES`,
`IRCU_MODULE_COMPILE_{DEFINITIONS,OPTIONS}`, or `IRCU_MODULE_SKIP` to opt out
when a dependency is missing; the core's build files never learn about it. Sources live in `modules/<type>/` (`commands`,
`modes`, `hooks`, `workers`; a type is just a directory) as either
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

**Hooks** (`include/hooks.h`, `ircd/hooks.c`). A closed enum of lifecycle points
modules attach to. Points named `HOOK_*_PRE_*` run before the server acts and may
veto (`HOOK_DENY`) or, for messages, rewrite; the rest are after-the-fact
notifications whose return value is ignored. All hooks run inline on the main thread.

**Design docs.** `doc/proposals/` holds the accepted designs for the module API
(001), the multithreading direction (002, in Spanish — option B is what
`ircd/worker.c` implements) and the channel modes by module (003, in Spanish);
read the relevant one before changing either subsystem. Other useful docs:
`doc/p10.html` (protocol), `doc/readme.modules`, `doc/readme.workers`,
`doc/readme.database`, `doc/features.txt`, `doc/api/` (subsystem notes; `Doxyfile` at the root generates reference docs).

## Conventions

- C99, K&R-ish 2-space style; `.indent.pro` and `doc/readme.indent` define the
  formatting. Doxygen comments (`/** ... */`) on public declarations.
- Add a new command as `ircd/m_<cmd>.c` + an `msgtab[]` entry + a prototype in
  `include/handlers.h`; add the source to `ircd/CMakeLists.txt`.
- Generated at build time, never edited: `ircd_parser.c/h` (bison), `chattr.tab.c`
  (via `table_gen`), `version.c`. `include/patchlist.h` is written by `ircd-patch`.
