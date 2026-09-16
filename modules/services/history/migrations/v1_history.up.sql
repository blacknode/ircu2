-- The message store.
--
-- One table, partitioned by month.  The partition key has to be part of
-- every unique constraint PostgreSQL will accept on a partitioned table,
-- so the key of a message is (sent_at, msgid) rather than msgid alone --
-- which is exactly why sent_at has to be the *network's* idea of when the
-- message was sent and not each server's.  Every server that delivers a
-- message writes it, and the key is what makes the second, third and
-- fourth write a no-op instead of a duplicate.

CREATE TABLE IF NOT EXISTS message (
  sent_at           TIMESTAMPTZ NOT NULL,
  msgid             TEXT        NOT NULL,
  kind              SMALLINT    NOT NULL,
  is_channel        BOOLEAN     NOT NULL,
  target            TEXT        NOT NULL,
  target_canon      TEXT        NOT NULL,
  sender_nick       TEXT        NOT NULL,
  sender_account    TEXT,
  recipient_account TEXT,
  body              TEXT        NOT NULL,
  body_search       TSVECTOR
    GENERATED ALWAYS AS (to_tsvector('simple', body)) STORED,
  PRIMARY KEY (sent_at, msgid)
) PARTITION BY RANGE (sent_at);

COMMENT ON COLUMN message.target IS
  'Channel or nickname as the sender addressed it; shown, never matched on.';
COMMENT ON COLUMN message.sender_account IS
  'The nickname the sender had proved was theirs, lower-cased the way IRC '
  'lower-cases a nickname, or NULL.  An account is a nickname, so this is a '
  'nickname too; it is canonical because a user wears whichever spelling '
  'they typed.';
COMMENT ON COLUMN message.recipient_account IS
  'NULL for a channel message.  A direct message is stored only when both '
  'ends had proved their nicknames, because otherwise there is nobody it '
  'could later be shown to.';

-- A safety net, not a place to keep things: the module creates this
-- month's and next month's partitions ahead of time, and a message that
-- somehow arrives outside both is better landed here than refused.
CREATE TABLE IF NOT EXISTS message_default PARTITION OF message DEFAULT;

-- BRIN over time.  The rows inside one month's partition arrive in time
-- order, which is the one case BRIN is made for: a few pages of index for
-- a table of any size.
CREATE INDEX IF NOT EXISTS message_sent_at_brin
  ON message USING BRIN (sent_at);

-- What a room reads: one channel, newest first.
CREATE INDEX IF NOT EXISTS message_channel_idx
  ON message (target_canon, sent_at DESC) WHERE is_channel;

-- What a conversation reads.  Two indexes because either end may be the
-- sender, and a query that has to be written as an OR of the two needs
-- both to be indexed or it reads the whole month.
CREATE INDEX IF NOT EXISTS message_dm_idx
  ON message (sender_account, recipient_account, sent_at DESC)
  WHERE NOT is_channel;
CREATE INDEX IF NOT EXISTS message_dm_rev_idx
  ON message (recipient_account, sender_account, sent_at DESC)
  WHERE NOT is_channel;

-- Search.  'simple' rather than a language configuration: the server does
-- not know what language a channel speaks, and stemming the wrong one is
-- worse than not stemming at all.
CREATE INDEX IF NOT EXISTS message_search_idx
  ON message USING GIN (body_search);

-- Create the partition for the month containing p_ts, if it is missing.
--
-- Everything is anchored to UTC explicitly rather than to the database's
-- TimeZone: two servers in different time zones writing the same message
-- must agree on which month it belongs to.
--
-- The advisory lock is transaction-scoped and this function is called as a
-- statement of its own, so it is held for exactly that statement (see
-- doc/readme.database): two servers racing to create the same partition
-- means one of them waits and then finds it already there.
CREATE OR REPLACE FUNCTION history_ensure_partition(p_ts TIMESTAMPTZ)
RETURNS TEXT AS $$
DECLARE
  v_start TIMESTAMPTZ;
  v_end   TIMESTAMPTZ;
  v_name  TEXT;
BEGIN
  v_start := date_trunc('month', p_ts AT TIME ZONE 'UTC') AT TIME ZONE 'UTC';
  v_end   := (date_trunc('month', p_ts AT TIME ZONE 'UTC')
              + INTERVAL '1 month') AT TIME ZONE 'UTC';
  v_name  := 'message_' || to_char(v_start AT TIME ZONE 'UTC', 'YYYYMM');

  PERFORM pg_advisory_xact_lock(hashtext('history:partition:' || v_name));

  IF to_regclass(quote_ident(v_name)) IS NULL THEN
    EXECUTE format(
      'CREATE TABLE %I PARTITION OF message FOR VALUES FROM (%L) TO (%L)',
      v_name, v_start, v_end);
  END IF;

  RETURN v_name;
END;
$$ LANGUAGE plpgsql;

-- Forget everything one account said or was told.
--
-- A direct message is one row, not two, so forgetting an account takes the
-- other end's copy of the conversation with it.  That is the answer the
-- law asks for and not a side effect: what is being deleted is the
-- message, and there is only one of it.
CREATE OR REPLACE FUNCTION history_forget(p_account TEXT)
RETURNS BIGINT AS $$
DECLARE
  v_rows BIGINT;
BEGIN
  DELETE FROM message
   WHERE sender_account = p_account
      OR recipient_account = p_account;
  GET DIAGNOSTICS v_rows = ROW_COUNT;
  RETURN v_rows;
END;
$$ LANGUAGE plpgsql;

-- Drop everything older than p_before.
--
-- A whole partition that ends at or before the cutoff is dropped, which
-- costs nothing whatever it holds; what is left straddling the cutoff, and
-- whatever landed in the default partition, is deleted row by row.  The
-- return value is how many partitions were dropped.
CREATE OR REPLACE FUNCTION history_purge(p_before TIMESTAMPTZ)
RETURNS INTEGER AS $$
DECLARE
  v_part   RECORD;
  v_bound  TEXT;
  v_to     TIMESTAMPTZ;
  v_count  INTEGER := 0;
BEGIN
  FOR v_part IN
    SELECT c.oid, c.relname, pg_get_expr(c.relpartbound, c.oid) AS bound
      FROM pg_class c
      JOIN pg_inherits i ON i.inhrelid = c.oid
      JOIN pg_class p ON p.oid = i.inhparent
     WHERE p.relname = 'message'
  LOOP
    v_bound := v_part.bound;

    -- The default partition has no bounds to compare; it is swept below.
    CONTINUE WHEN v_bound IS NULL OR v_bound = 'DEFAULT';

    v_to := substring(v_bound from 'TO \(''([^'']+)''\)')::TIMESTAMPTZ;
    CONTINUE WHEN v_to IS NULL OR v_to > p_before;

    EXECUTE format('DROP TABLE %I', v_part.relname);
    v_count := v_count + 1;
  END LOOP;

  DELETE FROM message WHERE sent_at < p_before;

  RETURN v_count;
END;
$$ LANGUAGE plpgsql;
