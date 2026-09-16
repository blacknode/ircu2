-- Make a month's partition creatable after the default has taken rows for it.
--
-- The default partition is a safety net: a message that arrives for a month
-- with no partition yet is better landed there than refused.  But PostgreSQL
-- will not create a partition whose range the default already holds rows for
-- -- it would have to move them, and it declines to guess -- so the net
-- turned into a trap.  Every message stored before the schema was migrated,
-- or in the seconds before the module's first maintenance pass, permanently
-- blocked its own month:
--
--   ERROR: updated partition constraint for default partition
--          "message_default" would be violated by some row
--
-- and that month's traffic then stayed in the default for ever, where
-- retention cannot drop it as a partition and the time-ordered locality BRIN
-- depends on is gone.
--
-- So moving them is what this does.  The rows come out of the default, the
-- partition is created, and they go back in through the parent, which now
-- routes them to it.  The whole thing is one statement's implicit
-- transaction, under the same advisory lock as before, so a second server
-- doing it at the same moment waits and then finds the partition already
-- there.
--
-- The column list is written out rather than SELECT *: body_search is
-- generated, and a generated column cannot be inserted into.

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

  IF to_regclass(quote_ident(v_name)) IS NOT NULL THEN
    RETURN v_name;
  END IF;

  CREATE TEMP TABLE history_rescued ON COMMIT DROP AS
    SELECT sent_at, msgid, kind, is_channel, target, target_canon,
           sender_nick, sender_account, recipient_account, body, sender_prefix
      FROM message_default
     WHERE sent_at >= v_start AND sent_at < v_end;

  DELETE FROM message_default
   WHERE sent_at >= v_start AND sent_at < v_end;

  EXECUTE format(
    'CREATE TABLE %I PARTITION OF message FOR VALUES FROM (%L) TO (%L)',
    v_name, v_start, v_end);

  INSERT INTO message (sent_at, msgid, kind, is_channel, target, target_canon,
                       sender_nick, sender_account, recipient_account, body,
                       sender_prefix)
    SELECT sent_at, msgid, kind, is_channel, target, target_canon,
           sender_nick, sender_account, recipient_account, body, sender_prefix
      FROM history_rescued;

  DROP TABLE history_rescued;

  RETURN v_name;
END;
$$ LANGUAGE plpgsql;
