-- Run on existing D1 databases after pull:
-- npx wrangler d1 execute crymore --remote --file=./schema-migrate-avatar.sql

ALTER TABLE users ADD COLUMN avatar_key TEXT;
