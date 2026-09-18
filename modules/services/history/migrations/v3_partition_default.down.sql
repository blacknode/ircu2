-- Back to the version that refuses when the default holds the month.
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
