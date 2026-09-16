-- How far somebody has read, per conversation.
--
-- The thing that turns a history into an inbox: without it a client
-- reconnecting can fetch everything it missed but cannot tell which of it
-- it has already seen, so everything is either unread or nothing is.
--
-- Keyed by account and by target, not by connection: a person reads on
-- their phone and expects their laptop to know.  The account is a
-- nickname, so this is a nickname too, canonical.
--
-- The marker is a timestamp rather than a msgid.  "Everything up to here"
-- is a point in time, and a message arriving late -- from a server that
-- was split -- is behind the marker if it was sent behind it, which is
-- the answer a person means.  A msgid would name one message and say
-- nothing about the ones around it.
CREATE TABLE IF NOT EXISTS read_marker (
  account      TEXT        NOT NULL,
  target_canon TEXT        NOT NULL,
  marker       TIMESTAMPTZ NOT NULL,
  updated_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (account, target_canon)
);

COMMENT ON TABLE read_marker IS
  'How far one account has read one conversation.  Per account, not per '
  'connection: a person reads on their phone and expects their laptop to '
  'know.';

-- Setting a marker only ever moves it forward.  Two clients of the same
-- person race constantly -- one is catching up while the other is at the
-- bottom -- and a marker that could move backwards would make messages
-- unread again every time the slower one reported in.
CREATE OR REPLACE FUNCTION read_marker_set(p_account TEXT,
                                           p_target TEXT,
                                           p_marker TIMESTAMPTZ)
RETURNS TIMESTAMPTZ AS $$
DECLARE
  v_now TIMESTAMPTZ;
BEGIN
  INSERT INTO read_marker (account, target_canon, marker)
       VALUES (p_account, p_target, p_marker)
  ON CONFLICT (account, target_canon) DO UPDATE
          SET marker = GREATEST(read_marker.marker, EXCLUDED.marker),
              updated_at = now()
    RETURNING marker INTO v_now;

  RETURN v_now;
END;
$$ LANGUAGE plpgsql;
