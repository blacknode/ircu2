DROP INDEX IF EXISTS message_msgid_idx;
ALTER TABLE message DROP COLUMN IF EXISTS sender_prefix;
