-- What reading the history needs and writing it did not.

-- The prefix the sender was wearing, nick!user@host.
--
-- A transcript has to be self-contained: a message is shown back weeks
-- later, when the person may have changed nickname, changed host or never
-- come back, and reconstructing a prefix from whoever holds that nickname
-- today would put words in the wrong mouth.  Old rows have NULL and fall
-- back to the bare nickname, which is a legal prefix.
ALTER TABLE message ADD COLUMN IF NOT EXISTS sender_prefix TEXT;

-- CHATHISTORY names a point in the conversation by msgid, so a msgid has
-- to be findable.  The primary key is (sent_at, msgid) -- it has to
-- include the partition key -- which does not answer "where is this
-- message" on its own.
CREATE INDEX IF NOT EXISTS message_msgid_idx ON message (msgid);
