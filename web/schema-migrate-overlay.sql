-- Run on existing D1: npx wrangler d1 execute crymore --remote --file=./schema-migrate-overlay.sql

ALTER TABLE releases ADD COLUMN overlay_r2_key TEXT;
ALTER TABLE releases ADD COLUMN overlay_dek_b64 TEXT;
