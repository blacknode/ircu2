-- Deleting one message, and what pointed at it.
--
-- A reaction to a message that is gone is a reference to nothing, so the
-- reactions go with it.  A *reply* does not: a reply is a message of its
-- own, somebody else said it, and redacting one message is not permission
-- to redact the conversation that followed.  Its reply_to is left pointing
-- at a message that is no longer there, which is exactly what happened.
CREATE OR REPLACE FUNCTION history_redact(p_msgid TEXT)
RETURNS BIGINT AS $$
DECLARE
  v_rows BIGINT;
BEGIN
  DELETE FROM message
   WHERE msgid = p_msgid
      OR (kind = 2 AND reply_to = p_msgid);
  GET DIAGNOSTICS v_rows = ROW_COUNT;
  RETURN v_rows;
END;
$$ LANGUAGE plpgsql;
