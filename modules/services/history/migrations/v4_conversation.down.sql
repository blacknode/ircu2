DROP INDEX IF EXISTS message_reply_to_idx;
ALTER TABLE message DROP COLUMN IF EXISTS reply_to;
