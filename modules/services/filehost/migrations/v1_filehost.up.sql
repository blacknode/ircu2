-- What is stored about a file.  The bytes are on disk; this is everything
-- else, and it is what makes listing, quota and retention possible without
-- walking a directory that may hold a million objects.

CREATE TABLE IF NOT EXISTS file (
  -- Sixteen characters of base 62, minted by the server.  It is what
  -- appears in the URL, so it is also what the module refuses to take
  -- from a client in any other shape: an identifier that could be a path
  -- is a path.
  id           text        PRIMARY KEY,
  -- The account that uploaded it, as the network's services named it.
  -- The only durable handle on a person there is: a bare nickname would
  -- file somebody's uploads under whoever wears the name next week.
  account      text        NOT NULL,
  -- The channel or nickname it was meant for, as the uploader named it.
  -- Kept for the record, not enforced: a link is a link once it is out.
  target       text        NOT NULL DEFAULT '',
  -- As the uploader called it, sanitised to one path component.
  name         text        NOT NULL,
  content_type text        NOT NULL,
  size         bigint      NOT NULL,
  created_at   timestamptz NOT NULL DEFAULT now(),
  -- NULL means keep for ever, which is what a retention of zero asks for.
  expires_at   timestamptz
);

-- Listing an account's files, newest first, is the one query a person
-- makes about themselves.
CREATE INDEX IF NOT EXISTS file_account_idx ON file (account, created_at DESC);

-- And the sweep, which asks only about the rows that can expire.
CREATE INDEX IF NOT EXISTS file_expires_idx ON file (expires_at)
  WHERE expires_at IS NOT NULL;
