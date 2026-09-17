-- When a message was last changed, or NULL if it never was.
--
-- The identifier does not change with an edit, which is the whole reason
-- an edit is not simply another message: the replies and the reactions
-- point at it, and a message that was rewritten is still the one they are
-- about.  So there is nowhere else for "this is not what was sent" to
-- live, and a client being handed the text a second time has no other way
-- to know.
--
-- body_search is a generated column, so it follows the body without this
-- migration having anything to say about it.

ALTER TABLE message ADD COLUMN IF NOT EXISTS edited_at TIMESTAMPTZ;

COMMENT ON COLUMN message.edited_at IS
  'When the author last rewrote this message, or NULL.  Only the author '
  'ever can: an operator may redact a message, which leaves a hole '
  'everybody can see, but nobody may make somebody else''s message say '
  'something they did not say.';
