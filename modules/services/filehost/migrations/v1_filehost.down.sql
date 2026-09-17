-- The rows go; the objects on disk do not.  A migration that deleted
-- files would be a migration that cannot be undone, and reverting a
-- schema is not the same act as throwing away what people uploaded.
DROP TABLE IF EXISTS file;
