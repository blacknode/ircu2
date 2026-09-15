/* test_stub.c - support stubs for test programs */

#include "client.h"
#include "ircd_events.h"
#include "ircd_log.h"
#include "s_debug.h"
#include <stdarg.h>
#include <stdio.h>

struct Client me;
int log_inassert;

void
log_write(enum LogSys subsys, enum LogLevel severity, unsigned int flags,
          const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

void
debug(int level, const char *form, ...)
{
    va_list args;

    va_start(args, form);
    vfprintf(stdout, form, args);
    fputc('\n', stdout);
    va_end(args);
}

int
exit_client(struct Client *cptr, struct Client *victim, struct Client *killer,
            const char *comment)
{
    Debug((DEBUG_LIST, "exit_client(%p, %p, %p, \"%s\")\n", cptr, victim, killer, comment));
    return 0;
}

/* Stub for migration_core_start().
 *
 * db.c calls it when a driver registers, so that a database module loaded
 * by hand still gets the migrations table created.  The real one lives in
 * migration_run.c, which reaches into the event loop, the client list and
 * send.c; a test that links db.c or module.c wants the validation half of
 * the migration subsystem (migration.c) and none of that.
 */
void migration_core_start(void)
{
}

/* Stubs for the timer hooks.c arms behind a suspended operation.
 *
 * What the deadline does is tested by calling hook_pending_expire() with
 * the time the test wants; getting there through an event loop would be
 * testing ircd_events.c, and linking it would bring the engines with it.
 */
time_t CurrentTime;

struct Timer *timer_init(struct Timer *timer)
{
    return timer;
}

void timer_add(struct Timer *timer, EventCallBack call, void *data,
               enum TimerType type, time_t value)
{
    Debug((DEBUG_LIST, "timer_add(%p, %p, %p, %d, %ld)\n", timer, call, data,
           (int) type, (long) value));
}

void timer_del(struct Timer *timer)
{
    Debug((DEBUG_LIST, "timer_del(%p)\n", timer));
}
