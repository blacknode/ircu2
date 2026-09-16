-- What a message is about, beyond its own text.
--
-- Phase 3 of the roadmap is threads, reactions, redaction and read markers,
-- and all of them are the same idea: something that refers back to a message
-- by the name the whole network knows it by.  That name already exists, so
-- what is missing is somewhere to write down the reference.

-- The message this one replies to, or the one it reacts to.
--
-- It is the client's claim, kept verbatim: "this is a reply to that" is
-- exactly the sort of thing only the person writing it can say, and the
-- server has no way to be more right about it.  No foreign key for the same
-- reason a message may reply to one the retention already dropped, or to one
-- from a server this one never heard from.
ALTER TABLE message ADD COLUMN IF NOT EXISTS reply_to TEXT;

-- Reading a thread is "everything that points at this message", which is
-- the one access shape the indexes above do not serve.
CREATE INDEX IF NOT EXISTS message_reply_to_idx
  ON message (reply_to, sent_at) WHERE reply_to IS NOT NULL;

-- A reaction is a TAGMSG that carries one, so it is a message like any
-- other: its own msgid, its own time, reply_to naming what it is a
-- reaction to, and the reaction itself in body.  Storing it as a row
-- rather than as a column on the message it decorates is what makes
-- removing one, and reading them back in order, the same operations as
-- for anything else -- and what keeps a message with four hundred
-- reactions from being a four-hundred-element array nobody can index.
COMMENT ON COLUMN message.reply_to IS
  'The message this one replies to, or that a kind=2 reaction reacts to. '
  'The client''s claim, kept verbatim.';
