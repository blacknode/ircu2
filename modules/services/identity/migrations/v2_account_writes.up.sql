-- The writes, as functions, because the locking is the point.
--
-- Counting an address's accounts and then inserting one is a race the
-- moment there are two servers: both count two, both insert, the address
-- ends up with four.  So the count and the insert have to be one
-- transaction, and the transaction has to start by taking a lock on the
-- address.
--
-- They are functions and not statements the module sends because db.h has
-- no transactions: it runs one statement on a pooled connection, which is
-- the right shape for everything else it does.  A statement sent on its
-- own runs inside an implicit transaction, so pg_advisory_xact_lock()
-- inside a function called by one statement is held for exactly that
-- statement and released when it ends -- including when it ends by the
-- server dying, which is why the lock is the _xact_ one and not a session
-- lock that would leave an address blocked forever.
--
-- The UNIQUE indexes are still there.  They are what catches somebody
-- inserting by hand, which no lock can.

CREATE OR REPLACE FUNCTION account_register(
  p_email text,
  p_hash  text,
  p_nick  text,
  p_canon text,
  p_max   integer)
RETURNS TABLE (status text, account text, is_new boolean) AS $$
DECLARE
  v_id    bigint;
  v_count integer;
  v_new   boolean := false;
BEGIN
  -- The address first and the nickname second, always in that order:
  -- two transactions taking the same two locks in opposite orders is a
  -- deadlock, and the only thing that stops it is that nobody writes the
  -- other order.
  PERFORM pg_advisory_xact_lock(hashtext('ident:' || p_email));
  PERFORM pg_advisory_xact_lock(hashtext('nick:' || p_canon));

  SELECT id INTO v_id FROM identity WHERE email = p_email;

  IF v_id IS NULL THEN
    INSERT INTO identity (email, password_hash) VALUES (p_email, p_hash)
      RETURNING id INTO v_id;
    v_new := true;
  END IF;

  IF EXISTS (SELECT 1 FROM account WHERE nick_canon = p_canon) THEN
    RETURN QUERY SELECT 'exists'::text, NULL::text, v_new;
    RETURN;
  END IF;

  SELECT count(*) INTO v_count FROM account WHERE identity_id = v_id;

  IF p_max > 0 AND v_count >= p_max THEN
    RETURN QUERY SELECT 'limit'::text, NULL::text, v_new;
    RETURN;
  END IF;

  -- The first account an address registers is its default, so that
  -- logging in without naming one always has an answer.
  INSERT INTO account (identity_id, nick, nick_canon, is_default)
    VALUES (v_id, p_nick, p_canon, v_count = 0);

  RETURN QUERY SELECT 'ok'::text, p_nick, v_new;
END;
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION account_drop(p_email text, p_canon text)
RETURNS TABLE (status text, account text) AS $$
DECLARE
  v_id      bigint;
  v_nick    text;
  v_default boolean;
BEGIN
  PERFORM pg_advisory_xact_lock(hashtext('ident:' || p_email));
  PERFORM pg_advisory_xact_lock(hashtext('nick:' || p_canon));

  SELECT id INTO v_id FROM identity WHERE email = p_email;

  IF v_id IS NULL THEN
    RETURN QUERY SELECT 'nosuch'::text, NULL::text;
    RETURN;
  END IF;

  DELETE FROM account
   WHERE identity_id = v_id AND nick_canon = p_canon
   RETURNING nick, is_default INTO v_nick, v_default;

  IF v_nick IS NULL THEN
    RETURN QUERY SELECT 'nosuch'::text, NULL::text;
    RETURN;
  END IF;

  -- An address with accounts and no default is an address that cannot log
  -- in without naming one, which is a state nothing else in the model
  -- knows how to explain.  The oldest survivor takes over.
  IF v_default THEN
    UPDATE account SET is_default = true
     WHERE id = (SELECT id FROM account WHERE identity_id = v_id
                  ORDER BY created_at, id LIMIT 1);
  END IF;

  RETURN QUERY SELECT 'ok'::text, v_nick;
END;
$$ LANGUAGE plpgsql;
