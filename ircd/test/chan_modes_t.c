/* chan_modes_t.c - Test the run-time channel mode register.
 *
 * Covers what chan_modes.c promises to the modules that register channel
 * modes: the bit follows from the letter and from nothing else, the core
 * modes are seeded once and in the letters' own order, only registered
 * (non-core) modes can be removed, and removing one takes the mode off
 * every channel that still carries it and announces the change -- to the
 * members and to the network, because a channel belongs to the network.
 */

#include "channel.h"
#include "client.h"
#include "ircd.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/** Core modes seeded by channel_init_chan_modes(), in bit order. */
static const char core_modes[] = "ACDMRUZbcdiklmnoprstuvz";

/* Defined by chan_modes_stub.c. */
extern int stub_modebuf_calls;
extern struct Channel* stub_modebuf_channel;
extern chanmode_t stub_modebuf_mode;
extern unsigned int stub_modebuf_dest;
extern int stub_oplevels;

/** Number of modes currently registered. */
static unsigned int mode_count(void)
{
  const struct ChanMode* cm;
  unsigned int n = 0;

  for (cm = channel_chan_modes(); cm; cm = cm->next)
    n++;

  return n;
}

/** Render the registered letters, in list order. */
static const char* mode_chars(void)
{
  static char buf[CHANMODE_CHARS_LEN];
  const struct ChanMode* cm;
  size_t len = 0;

  for (cm = channel_chan_modes(); cm; cm = cm->next)
    buf[len++] = cm->c;
  buf[len] = '\0';

  return buf;
}

static void test_init(void)
{
  const struct ChanMode* cm;

  channel_init_chan_modes();

  assert(mode_count() == strlen(core_modes));

  /* The list is ordered by bit, which is the letters' own order: two
   * servers must render the same channel the same way, whatever order
   * their modes were registered in.
   */
  assert(0 == strcmp(mode_chars(), core_modes));

  for (cm = channel_chan_modes(); cm; cm = cm->next)
    assert(cm->flag == channel_chan_mode_flag(cm->c));

  assert(channel_chan_modes()->count == mode_count());

  printf("Passed: the core modes are seeded, in bit order\n");
}

static void test_init_is_idempotent(void)
{
  unsigned int before = mode_count();

  channel_init_chan_modes();
  assert(mode_count() == before);

  printf("Passed: seeding twice registers nothing twice\n");
}

/** The bit is a pure function of the letter, and of nothing else. */
static void test_flag_follows_the_letter(void)
{
  assert(channel_chan_mode_flag('A') == (BITSET << 0));
  assert(channel_chan_mode_flag('Z') == (BITSET << 25));
  assert(channel_chan_mode_flag('a') == (BITSET << 26));
  assert(channel_chan_mode_flag('z') == (BITSET << 51));

  /* Every letter lands below the reserved bits, so no mode can ever
   * collide with a direction or with the ModeBuf bookkeeping.
   */
  assert(!(channel_chan_mode_flag('z') & CHANMODE_RESERVED));
  assert(MODE_ADD & CHANMODE_RESERVED);
  assert(MODE_DEL & CHANMODE_RESERVED);

  assert(channel_chan_mode_flag('0') == 0);
  assert(channel_chan_mode_flag('[') == 0);
  assert(channel_chan_mode_flag('+') == 0);

  /* And the core constants agree with the rule. */
  assert(MODE_CHANOP == channel_chan_mode_flag('o'));
  assert(MODE_VOICE == channel_chan_mode_flag('v'));
  assert(MODE_SECRET == channel_chan_mode_flag('s'));

  printf("Passed: the bit follows from the letter\n");
}

static void test_check_rejects_bad_modes(void)
{
  assert(CMODE_INVALID_MODE ==
         channel_check_chan_mode('1', channel_chan_mode_flag('1')));
  assert(CMODE_INVALID_MODE == channel_check_chan_mode('W', 0));

  /* A flag that does not match its letter is a caller that computed the
   * bit some other way; the register has no use for it.
   */
  assert(CMODE_INVALID_MODE ==
         channel_check_chan_mode('W', channel_chan_mode_flag('X')));
  assert(CMODE_INVALID_MODE == channel_check_chan_mode('W', MODE_ADD));

  /* Taken by the core. */
  assert(CMODE_ALREADY_EXISTS ==
         channel_check_chan_mode('o', channel_chan_mode_flag('o')));

  assert(0 == channel_check_chan_mode('W', channel_chan_mode_flag('W')));

  printf("Passed: a mode has to claim a free letter\n");
}

static void test_append(void)
{
  unsigned int before = mode_count();
  const struct ChanMode* cm;

  assert(CMODE_APPEND_OK ==
         channel_append_chan_mode('W', channel_chan_mode_flag('W')));
  assert(mode_count() == before + 1);

  cm = channel_find_chan_mode('W');
  assert(cm != NULL);
  assert(cm->flag == channel_chan_mode_flag('W'));
  assert(cm->attr == 0);  /* a module's mode is a plain channel flag */
  assert(cm->alt == 0);

  /* It went in among the letters, not at the end: 'W' sorts after 'U'
   * and before 'Z'.
   */
  assert(0 == strcmp(mode_chars(), "ACDMRUWZbcdiklmnoprstuvz"));

  /* Twice is once. */
  assert(CMODE_ALREADY_EXISTS ==
         channel_append_chan_mode('W', channel_chan_mode_flag('W')));
  assert(mode_count() == before + 1);

  printf("Passed: a registered mode joins the list in bit order\n");
}

static void test_remove_rejects_core_and_unknown(void)
{
  assert(CMODE_CORE_MODE == channel_remove_chan_mode('o'));
  assert(CMODE_CORE_MODE == channel_remove_chan_mode('z'));
  assert(CMODE_UNKNOWN_MODE == channel_remove_chan_mode('Y'));
  assert(CMODE_INVALID_MODE == channel_remove_chan_mode('1'));

  assert(channel_find_chan_mode('o') != NULL);

  printf("Passed: the core's modes are not a module's to remove\n");
}

/** Removing a mode takes it off the channels that carry it, and says so. */
static void test_remove_clears_the_mode_from_channels(void)
{
  struct Channel with, without;
  chanmode_t flag = channel_chan_mode_flag('W');

  memset(&with, 0, sizeof(with));
  memset(&without, 0, sizeof(without));
  with.next = &without;
  GlobalChannelList = &with;

  SetCFlag(&with, flag);
  SetCFlag(&without, MODE_MODERATED);

  stub_modebuf_calls = 0;
  assert(CMODE_REMOVE_OK == channel_remove_chan_mode('W'));

  /* Announced exactly once: on the channel that had it. */
  assert(stub_modebuf_calls == 1);
  assert(stub_modebuf_channel == &with);
  assert(stub_modebuf_mode == (MODE_DEL | flag));

  /* To the members and to the network both: the mode is gone everywhere,
   * not just here.
   */
  assert(stub_modebuf_dest & MODEBUF_DEST_CHANNEL);
  assert(stub_modebuf_dest & MODEBUF_DEST_SERVER);

  assert(!HasCFlag(&with, flag));
  assert(HasCFlag(&without, MODE_MODERATED));

  assert(channel_find_chan_mode('W') == NULL);

  /* The letter is free again, and comes back as the same bit. */
  assert(CMODE_APPEND_OK == channel_append_chan_mode('W', flag));
  assert(channel_find_chan_mode('W')->flag == flag);
  assert(CMODE_REMOVE_OK == channel_remove_chan_mode('W'));

  GlobalChannelList = NULL;

  printf("Passed: removing a mode strips it and announces the change\n");
}

/** What the server advertises is built from the register, not fixed. */
static void test_advertised_modes(void)
{
  assert(0 == strcmp(channel_chan_mode_chars(), core_modes));
  assert(0 == strcmp(channel_chan_mode_param_chars(), "AUbklov"));
  assert(0 == strcmp(channel_chanmodes_supported(),
                     "b,AUk,l,CDMRZcdimnprstu"));

  /* Without oplevels the two passwords do not exist. */
  stub_oplevels = 0;
  assert(NULL == strchr(channel_chan_mode_chars(), 'A'));
  assert(NULL == strchr(channel_chan_mode_chars(), 'U'));
  assert(0 == strcmp(channel_chan_mode_param_chars(), "bklov"));
  assert(0 == strcmp(channel_chanmodes_supported(),
                     "b,k,l,CDMRZcdimnprstu"));
  stub_oplevels = 1;

  /* A module's mode is advertised too, in the group that takes no
   * argument -- otherwise clients are told it does not exist.
   */
  assert(CMODE_APPEND_OK ==
         channel_append_chan_mode('W', channel_chan_mode_flag('W')));
  assert(NULL != strchr(channel_chan_mode_chars(), 'W'));
  assert(NULL == strchr(channel_chan_mode_param_chars(), 'W'));
  assert(0 == strcmp(channel_chanmodes_supported(),
                     "b,AUk,l,CDMRWZcdimnprstu"));
  assert(CMODE_REMOVE_OK == channel_remove_chan_mode('W'));

  printf("Passed: MYINFO and CHANMODES come from the register\n");
}

int main(int argc, char* argv[])
{
  (void) argc;
  (void) argv;

  test_init();
  test_init_is_idempotent();
  test_flag_follows_the_letter();
  test_check_rejects_bad_modes();
  test_append();
  test_remove_rejects_core_and_unknown();
  test_remove_clears_the_mode_from_channels();
  test_advertised_modes();

  printf("Done.\n");
  return 0;
}
