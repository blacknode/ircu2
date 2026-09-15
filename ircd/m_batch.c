/*
 * IRC - Internet Relay Chat, ircd/m_batch.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 1, or (at your option)
 * any later version.
 */
/** @file
 * @brief IRCv3 BATCH from a client.
 *
 * The only batch a client may open is draft/multiline: a message too long
 * for one line, sent as several and put back together here.  The pieces
 * are collected by batch.c and relayed when the batch closes.
 *
 * The server's own batches -- labeled responses, and the one that carries
 * a long message back out -- never arrive here: they only ever go the
 * other way.
 */
#include "config.h"

#include "batch.h"
#include "capab.h"
#include "client.h"
#include "handlers.h"
#include "ircd.h"
#include "ircd_log.h"
#include "ircd_reply.h"
#include "ircd_string.h"
#include "msg.h"
#include "numeric.h"
#include "s_debug.h"
#include "send.h"

#include <string.h>

/** Handle a BATCH from a client.
 *
 * parv[1] is the identifier, prefixed by '+' to open or '-' to close.
 * Opening also takes parv[2], the type, and parv[3], the target.
 *
 * @param[in] cptr Client that sent us the message.
 * @param[in] sptr Original source of message.
 * @param[in] parc Number of arguments.
 * @param[in] parv Argument vector.
 * @see \ref m_functions
 */
int m_batch(struct Client* cptr, struct Client* sptr, int parc, char* parv[])
{
  const char* id;
  int err;

  assert(0 != cptr);
  assert(cptr == sptr);

  /* Without the capability there is nothing a client could mean by this,
   * and answering "unknown command" is the truth: this server does not
   * take batches from a client that did not ask to send them.
   */
  if (!CapHas(cli_active(sptr), CAP_MULTILINE))
    return send_reply(sptr, ERR_UNKNOWNCOMMAND, MSG_IRCBATCH);

  if (parc < 2 || EmptyString(parv[1]))
    return need_more_params(sptr, MSG_IRCBATCH);

  id = parv[1];

  if (*id == '+') {
    if (parc < 4)
      return need_more_params(sptr, MSG_IRCBATCH);

    err = batch_in_open(sptr, id + 1, parv[2], parv[3]);
    if (err)
      return send_reply(sptr, err, MSG_IRCBATCH);

    return 0;
  }

  if (*id == '-') {
    struct InBatch* batch;

    err = batch_in_close(sptr, id + 1);
    if (err)
      return send_reply(sptr, err, MSG_IRCBATCH);

    /* Found by batch_in_close() having succeeded; it is this client's and
     * it is the one named.
     */
    batch = batch_in_find(sptr);
    if (batch)
      batch_in_deliver(sptr, batch);

    return 0;
  }

  return send_reply(sptr, ERR_UNKNOWNCAPCMD, MSG_IRCBATCH);
}
