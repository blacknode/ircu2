/* user_modes_t.c - Test the run-time user mode registry.
 *
 * Covers what client.c promises to the modules that register user modes:
 * the core modes are seeded once, a mode has to claim a free character and
 * a free bit, only registered (non-core) modes can be removed, and removing
 * one takes the mode off every user that still carries it -- announcing the
 * change for the local ones -- so the bit can be handed out again.
 */

#include "client.h"
#include "ircd.h"
#include "user_flags.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/** Core modes seeded by client_init_user_modes(), in no particular order. */
static const char core_modes[] = "oOiwsdkgrRxzIc";

/** A bit no core mode uses, free for the tests to hand out. */
#define TEST_FLAG_ONE  (BITSET << 1)
/** A second free bit. */
#define TEST_FLAG_TWO  (BITSET << 2)

/** Number of modes currently registered. */
static unsigned int mode_count(void)
{
  const struct UserMode* um;
  unsigned int n = 0;

  for (um = client_user_modes(); um; um = um->next)
    n++;

  return n;
}

/** Find a mode by character, or NULL. */
static const struct UserMode* mode_find(char c)
{
  const struct UserMode* um;

  for (um = client_user_modes(); um; um = um->next)
    if (um->c == c)
      return um;

  return NULL;
}

static void test_init(void)
{
  const struct UserMode* um;
  const char* p;

  assert(NULL == client_user_modes() && "list starts out empty");

  client_init_user_modes();

  assert(mode_count() == strlen(core_modes));
  assert(client_user_modes()->count == strlen(core_modes)
         && "head node counts the list");

  for (p = core_modes; *p; p++) {
    um = mode_find(*p);
    assert(um != NULL && "core mode is registered");
    assert(um->flag != 0 && "core mode has a bit");
  }

  assert(mode_find('o')->flag == FLAG_OPER);
  assert(mode_find('c')->flag == FLAG_COMMONCHANS);

  printf("Passed: the core modes are seeded\n");
}

static void test_init_is_idempotent(void)
{
  unsigned int before = mode_count();

  client_init_user_modes();
  client_init_user_modes();

  assert(mode_count() == before && "seeding twice does not duplicate");

  printf("Passed: seeding is idempotent\n");
}

static void test_check_rejects_bad_modes(void)
{
  /* Only A-Z and a-z: the scandinavian alphabet is valid in nicks but not
   * in a mode string.
   */
  assert(UMODE_INVALID_MODE == client_check_user_mode('1', TEST_FLAG_ONE));
  assert(UMODE_INVALID_MODE == client_check_user_mode('[', TEST_FLAG_ONE));
  assert(UMODE_INVALID_MODE == client_check_user_mode('+', TEST_FLAG_ONE));
  assert(UMODE_INVALID_MODE == client_check_user_mode('\0', TEST_FLAG_ONE));
  /* A mode with no bit would be invisible to every test in the server. */
  assert(UMODE_INVALID_MODE == client_check_user_mode('Q', 0));

  /* The character and the bit must both be free: sharing either one means
   * two modes silently overwriting each other in cli_uflags().
   */
  assert(UMODE_ALREADY_EXISTS == client_check_user_mode('o', TEST_FLAG_ONE));
  assert(UMODE_ALREADY_EXISTS == client_check_user_mode('Q', FLAG_OPER));

  assert(0 == client_check_user_mode('Q', TEST_FLAG_ONE));

  printf("Passed: a mode must claim a free character and a free bit\n");
}

static void test_append(void)
{
  unsigned int before = mode_count();
  const struct UserMode* um;

  assert(UMODE_APPEND_OK == client_append_user_mode('Q', TEST_FLAG_ONE));

  um = mode_find('Q');
  assert(um != NULL);
  assert(um->flag == TEST_FLAG_ONE);
  assert(mode_count() == before + 1);
  assert(client_user_modes()->count == before + 1);

  /* Same character, same bit, and both at once. */
  assert(UMODE_ALREADY_EXISTS == client_append_user_mode('Q', TEST_FLAG_TWO));
  assert(UMODE_ALREADY_EXISTS == client_append_user_mode('W', TEST_FLAG_ONE));
  assert(UMODE_ALREADY_EXISTS == client_append_user_mode('Q', TEST_FLAG_ONE));
  assert(UMODE_INVALID_MODE == client_append_user_mode('4', TEST_FLAG_TWO));
  assert(mode_count() == before + 1 && "no failed append landed on the list");

  printf("Passed: a mode can be registered once\n");
}

static void test_remove_rejects_core_and_unknown(void)
{
  unsigned int before = mode_count();

  assert(UMODE_CORE_MODE == client_remove_user_mode('o'));
  assert(UMODE_CORE_MODE == client_remove_user_mode('c'));
  assert(UMODE_UNKNOWN_MODE == client_remove_user_mode('W'));
  assert(UMODE_INVALID_MODE == client_remove_user_mode('7'));
  assert(mode_count() == before);
  assert(mode_find('o') != NULL);

  printf("Passed: core and unknown modes are not removed\n");
}

static void test_remove_frees_the_slot(void)
{
  unsigned int before = mode_count();

  assert(UMODE_REMOVE_OK == client_remove_user_mode('Q'));
  assert(NULL == mode_find('Q'));
  assert(mode_count() == before - 1);
  assert(client_user_modes()->count == before - 1);

  /* Both the character and the bit are free again. */
  assert(0 == client_check_user_mode('Q', TEST_FLAG_ONE));

  printf("Passed: removing a mode frees its character and its bit\n");
}

/* --- Removing a mode that users still carry --------------------------- */

/** Clients the removal test walks, wired into GlobalClientList. */
static struct Client local_user;
static struct Client remote_user;
static struct Client a_server;
static struct Connection local_con;
static struct Connection remote_con;

/** Announcements made by send_umode_out(), recorded by the stub. */
extern int stub_umode_out_calls;
extern struct Client* stub_umode_out_client;
extern flag_t stub_umode_out_old;

static void build_client_list(void)
{
  cli_connect(&local_user) = &local_con;
  cli_from(&local_user) = &local_user;     /* MyUser() */
  cli_status(&local_user) = STAT_USER;

  cli_connect(&remote_user) = &remote_con;
  cli_from(&remote_user) = &a_server;      /* not ours to announce */
  cli_status(&remote_user) = STAT_USER;

  cli_status(&a_server) = STAT_SERVER;

  GlobalClientList = &local_user;
  cli_next(&local_user) = &remote_user;
  cli_next(&remote_user) = &a_server;
  cli_next(&a_server) = NULL;
}

static void test_remove_clears_the_mode_from_users(void)
{
  assert(UMODE_APPEND_OK == client_append_user_mode('Q', TEST_FLAG_ONE));

  build_client_list();
  SetUFlag(&local_user, TEST_FLAG_ONE | FLAG_INVISIBLE);
  SetUFlag(&remote_user, TEST_FLAG_ONE);
  SetUFlag(&a_server, TEST_FLAG_ONE); /* servers are skipped */

  stub_umode_out_calls = 0;
  assert(UMODE_REMOVE_OK == client_remove_user_mode('Q'));

  assert(!HasUFlag(&local_user, TEST_FLAG_ONE));
  assert(!HasUFlag(&remote_user, TEST_FLAG_ONE));
  assert(HasUFlag(&local_user, FLAG_INVISIBLE) && "other modes are untouched");
  assert(IsInvisible(&local_user));
  assert(HasUFlag(&a_server, TEST_FLAG_ONE) && "non-users are left alone");

  /* Only our own users are announced; every other server does the same for
   * the clients it is responsible for.
   */
  assert(1 == stub_umode_out_calls);
  assert(&local_user == stub_umode_out_client);
  assert(stub_umode_out_old & TEST_FLAG_ONE
         && "the snapshot still has the mode set");

  GlobalClientList = NULL;

  printf("Passed: removing a mode takes it off the users that carry it\n");
}

/** A client the server introduced on its own behalf, as modules/m_bot.c
 * does: its server is &me but its connection is &me's, so it is not
 * MyUser().  Nobody else will announce its modes, so this server must.
 */
static struct Client virtual_user;
static struct User virtual_user_user;
static struct Connection me_con;

static void test_remove_announces_virtual_users(void)
{
  assert(UMODE_APPEND_OK == client_append_user_mode('Q', TEST_FLAG_ONE));

  cli_connect(&me) = &me_con;
  cli_from(&me) = &me;

  cli_connect(&virtual_user) = &me_con;      /* shares &me's connection */
  cli_status(&virtual_user) = STAT_USER;
  cli_user(&virtual_user) = &virtual_user_user;
  virtual_user_user.server = &me;
  assert(!MyUser(&virtual_user));

  GlobalClientList = &virtual_user;
  cli_next(&virtual_user) = NULL;

  SetUFlag(&virtual_user, TEST_FLAG_ONE);

  stub_umode_out_calls = 0;
  assert(UMODE_REMOVE_OK == client_remove_user_mode('Q'));

  assert(!HasUFlag(&virtual_user, TEST_FLAG_ONE));
  assert(1 == stub_umode_out_calls);
  assert(&virtual_user == stub_umode_out_client);

  GlobalClientList = NULL;

  printf("Passed: removing a mode announces it for users this server "
         "introduced\n");
}

/* --- The flag macros -------------------------------------------------- */

static void test_flag_macros(void)
{
  struct Client cli;
  flag_t old;

  memset(&cli, 0, sizeof(cli));

  assert(!IsOper(&cli) && !IsInvisible(&cli) && !IsAccount(&cli));

  SetOper(&cli);
  SetInvisible(&cli);
  assert(IsOper(&cli) && IsInvisible(&cli));
  assert(!IsAccount(&cli) && "setting one mode does not set another");

  old = cli_uflags(&cli);
  assert(WasOper(old) && WasInvisible(old) && !WasAccount(old));
  assert(WasAnOper(old));

  ClearOper(&cli);
  assert(!IsOper(&cli));
  assert(IsInvisible(&cli) && "clearing one mode does not clear another");
  assert(WasOper(old) && "the snapshot is a copy, not an alias");

  SetTLS(&cli);
  assert(IsTLS(&cli));
  ClearTLS(&cli);
  assert(!IsTLS(&cli));

  SetAccount(&cli);
  assert(IsAccount(&cli));
  ClearAccount(&cli);
  assert(!IsAccount(&cli));

  printf("Passed: the user flag macros touch one mode at a time\n");
}

int main(int argc, char* argv[])
{
  (void) argc;
  (void) argv;

  test_init();
  test_init_is_idempotent();
  test_check_rejects_bad_modes();
  test_append();
  test_remove_rejects_core_and_unknown();
  test_remove_frees_the_slot();
  test_remove_clears_the_mode_from_users();
  test_remove_announces_virtual_users();
  test_flag_macros();

  printf("Done.\n");
  return 0;
}
