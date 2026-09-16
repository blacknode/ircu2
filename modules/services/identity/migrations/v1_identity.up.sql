-- The identity store: an address, and the accounts it holds.
--
-- An account IS a nickname (proposal 007), so there is no account name to
-- keep in step with anything: the nick is the account, and this table is
-- the only place that knows which address owns it.

CREATE TABLE IF NOT EXISTS identity (
  id               bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  -- Lower-cased by the module before it is written or compared, so that
  -- one address cannot be registered twice in different capitals.  The
  -- UNIQUE index is therefore a plain one and the lookup uses it.
  email            text        NOT NULL UNIQUE,
  -- $argon2id$v=19$m=...,t=...,p=...$salt$tag.  The costs travel inside
  -- the hash, so raising them does not invalidate what is stored.
  password_hash    text        NOT NULL,
  -- What SASL EXTERNAL matches on: the certificate fingerprint the server
  -- already has for the connection.  One per address, NULL for most.
  cert_fingerprint text        UNIQUE,
  mfa_secret       bytea,       -- AES-256-GCM, for a later mechanism
  sso_subject      text        UNIQUE,
  verified_at      timestamptz,
  created_at       timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS account (
  id           bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  identity_id  bigint      NOT NULL REFERENCES identity(id) ON DELETE CASCADE,
  -- As whoever registered it wrote it; this is what the network sees.
  nick         text        NOT NULL,
  -- Normalised the way the ircd normalises a nickname, which is not the
  -- way lower() does it: in IRC '[', ']' and '\' are the capitals of '{',
  -- '}' and '|', so lower() would allow two accounts where the network
  -- sees one nickname.  The module does the normalisation.
  nick_canon   text        NOT NULL UNIQUE,
  is_default   boolean     NOT NULL DEFAULT false,
  suspended_at timestamptz,
  created_at   timestamptz NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS account_identity_id_idx ON account (identity_id);

-- One default account per address.  A partial unique index rather than a
-- constraint, because "at most one row where is_default" is exactly what
-- it says and a trigger would be a second place to get it wrong.
CREATE UNIQUE INDEX IF NOT EXISTS account_one_default_idx
  ON account (identity_id) WHERE is_default;
