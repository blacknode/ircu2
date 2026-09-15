/* batch_t.c - Test labeled responses and the batches that carry them.
 *
 * The shape of the answer is fixed by the specification and this checks
 * each branch of it: nothing sent means a bare ACK, anything sent means a
 * batch, the batch opens on the first message rather than in advance, the
 * label rides on the opening line and not on the closing one, and a client
 * that did not negotiate both capabilities is answered the way it would
 * have been before it asked for anything.
 */

#include "batch.h"
#include "capab.h"
#include "client.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Defined by batch_stub.c. */
extern char stub_sent[16][256];
extern int stub_sent_count;
extern void stub_sent_reset(void);

/** A local client, and the connection that makes MyConnect() true. */
static struct Client cli;
static struct Connection con;
/** Another one, to check that a label belongs to one client only. */
static struct Client other;
static struct Connection other_con;

static void clients_init(void)
{
  memset(&cli, 0, sizeof(cli));
  memset(&con, 0, sizeof(con));
  con.con_client = &cli;
  cli.cli_connect = &con;
  strcpy(cli.cli_name, "labuser");

  memset(&other, 0, sizeof(other));
  memset(&other_con, 0, sizeof(other_con));
  other_con.con_client = &other;
  other.cli_connect = &other_con;
  strcpy(other.cli_name, "someone");
}

/** Give \a c the capabilities a labeled response needs. */
static void give_caps(struct Client *c, int batch, int labeled)
{
  CapClrAll(cli_active(c));
  if (batch)
    CapSet(cli_active(c), CAP_BATCH);
  if (labeled)
    CapSet(cli_active(c), CAP_LABELEDRESPONSE);
}

/** A command that sends nothing is answered with a bare ACK. */
static void test_ack_when_silent(void)
{
  give_caps(&cli, 1, 1);
  stub_sent_reset();

  label_begin(&cli, "abc");
  /* Nothing sent. */
  label_end();

  assert(stub_sent_count == 1);
  assert(0 == strcmp(stub_sent[0], "RAW ACK"));

  printf("ok - a command that answers nothing gets an ACK\n");
}

/** The batch opens on the first message, not in advance. */
static void test_batch_opens_lazily(void)
{
  give_caps(&cli, 1, 1);
  stub_sent_reset();

  label_begin(&cli, "abc");

  /* Before anything is sent there is no batch: that is what lets the
   * server decide between ACK and a batch without buffering a thing.
   */
  assert(stub_sent_count == 0);
  assert(batch_current(&cli) == 0);

  label_before_send(&cli);
  assert(stub_sent_count == 1);
  assert(0 == strncmp(stub_sent[0], "BATCH +", 7));
  assert(strstr(stub_sent[0], "labeled-response") != 0);

  /* Now the messages are inside it. */
  assert(batch_current(&cli) != 0);

  /* And a second message does not open a second batch. */
  label_before_send(&cli);
  assert(stub_sent_count == 1);

  label_end();
  assert(stub_sent_count == 2);
  assert(0 == strncmp(stub_sent[1], "BATCH -", 7));

  printf("ok - the batch opens on the first message (%s / %s)\n",
         stub_sent[0], stub_sent[1]);
}

/** The label rides on the opening line, and on nothing else. */
static void test_label_only_on_the_open(void)
{
  give_caps(&cli, 1, 1);
  stub_sent_reset();

  label_begin(&cli, "xyz");

  /* Before the batch exists, no message carries the label: the label is
   * not repeated on every reply, it names the batch.
   */
  assert(batch_label_tag(&cli) == 0);

  label_before_send(&cli);

  /* Inside the batch: batch tag, no label. */
  assert(batch_current(&cli) != 0);
  assert(batch_label_tag(&cli) == 0);

  label_end();

  /* And nothing is left set afterwards. */
  assert(batch_current(&cli) == 0);
  assert(batch_label_tag(&cli) == 0);

  printf("ok - the label names the batch, not every message in it\n");
}

/** A batch belongs to one client. */
static void test_other_clients_untouched(void)
{
  give_caps(&cli, 1, 1);
  give_caps(&other, 1, 1);
  stub_sent_reset();

  label_begin(&cli, "abc");
  label_before_send(&cli);

  assert(batch_current(&cli) != 0);
  assert(batch_current(&other) == 0);

  /* A message to somebody else does not open anything. */
  label_before_send(&other);
  assert(stub_sent_count == 1);

  label_end();

  printf("ok - a batch belongs to the client that asked for it\n");
}

/** Without both capabilities the client is answered as it always was. */
static void test_capabilities_required(void)
{
  /* labeled-response alone: the specification builds it on batches, and a
   * response of more than one message cannot be expressed without one.
   */
  give_caps(&cli, 0, 1);
  stub_sent_reset();
  label_begin(&cli, "abc");
  label_before_send(&cli);
  label_end();
  assert(stub_sent_count == 0);
  assert(batch_current(&cli) == 0);

  /* batch alone: nothing asked for a label. */
  give_caps(&cli, 1, 0);
  stub_sent_reset();
  label_begin(&cli, "abc");
  label_before_send(&cli);
  label_end();
  assert(stub_sent_count == 0);

  /* Neither. */
  give_caps(&cli, 0, 0);
  stub_sent_reset();
  label_begin(&cli, "abc");
  label_before_send(&cli);
  label_end();
  assert(stub_sent_count == 0);

  printf("ok - a client without both capabilities is answered as before\n");
}

/** An absent or empty label arms nothing. */
static void test_no_label(void)
{
  give_caps(&cli, 1, 1);

  stub_sent_reset();
  label_begin(&cli, 0);
  label_before_send(&cli);
  label_end();
  assert(stub_sent_count == 0);

  stub_sent_reset();
  label_begin(&cli, "");
  label_before_send(&cli);
  label_end();
  assert(stub_sent_count == 0);

  /* And label_end() on its own does nothing at all. */
  stub_sent_reset();
  label_end();
  assert(stub_sent_count == 0);

  printf("ok - a command with no label is left alone\n");
}

/** A client that goes away mid-response leaves nothing behind. */
static void test_client_exiting(void)
{
  give_caps(&cli, 1, 1);
  stub_sent_reset();

  label_begin(&cli, "abc");
  label_before_send(&cli);
  assert(batch_current(&cli) != 0);

  batch_client_exiting(&cli);

  /* No closing line is sent to a connection that is being freed, and
   * nothing is left pointing at it.
   */
  assert(batch_current(&cli) == 0);
  label_end();
  assert(stub_sent_count == 1);

  printf("ok - a client that leaves mid-response leaves nothing behind\n");
}

/** Identifiers do not repeat while the server runs. */
static void test_ids_differ(void)
{
  char first[64];

  give_caps(&cli, 1, 1);

  stub_sent_reset();
  label_begin(&cli, "a");
  label_before_send(&cli);
  strcpy(first, stub_sent[0]);
  label_end();

  stub_sent_reset();
  label_begin(&cli, "b");
  label_before_send(&cli);
  assert(0 != strcmp(first, stub_sent[0]));
  label_end();

  printf("ok - two batches do not share an identifier\n");
}

int main(void)
{
  clients_init();

  test_ack_when_silent();
  test_batch_opens_lazily();
  test_label_only_on_the_open();
  test_other_clients_untouched();
  test_capabilities_required();
  test_no_label();
  test_client_exiting();
  test_ids_differ();

  printf("ok - batch_t\n");
  return 0;
}
