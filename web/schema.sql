-- crymore.pw platform schema (D1 / SQLite)

CREATE TABLE IF NOT EXISTS users (
  id TEXT PRIMARY KEY,
  email TEXT NOT NULL UNIQUE,
  username TEXT NOT NULL UNIQUE COLLATE NOCASE,
  password_hash TEXT NOT NULL,
  banned INTEGER NOT NULL DEFAULT 0,
  avatar_key TEXT,
  created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS invite_codes (
  code TEXT PRIMARY KEY,
  max_uses INTEGER NOT NULL DEFAULT 1,
  uses INTEGER NOT NULL DEFAULT 0,
  expires_at INTEGER,
  note TEXT,
  created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS subscriptions (
  user_id TEXT PRIMARY KEY REFERENCES users(id),
  plan TEXT NOT NULL DEFAULT 'free',
  expires_at INTEGER NOT NULL,
  renewed_at INTEGER,
  stripe_customer_id TEXT,
  stripe_subscription_id TEXT
);

CREATE TABLE IF NOT EXISTS hwid_bindings (
  user_id TEXT NOT NULL REFERENCES users(id),
  hwid TEXT NOT NULL,
  first_seen INTEGER NOT NULL,
  last_seen INTEGER NOT NULL,
  PRIMARY KEY (user_id, hwid)
);

CREATE TABLE IF NOT EXISTS refresh_tokens (
  token_hash TEXT PRIMARY KEY,
  user_id TEXT NOT NULL REFERENCES users(id),
  hwid TEXT,
  expires_at INTEGER NOT NULL,
  revoked INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS releases (
  id TEXT PRIMARY KEY,
  version TEXT NOT NULL,
  channel TEXT NOT NULL DEFAULT 'stable',
  r2_key TEXT NOT NULL,
  file_name TEXT NOT NULL DEFAULT 'crymore-loader.zip',
  overlay_r2_key TEXT,
  overlay_dek_b64 TEXT,
  published_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_subscriptions_expires ON subscriptions(expires_at);
CREATE INDEX IF NOT EXISTS idx_refresh_user ON refresh_tokens(user_id);

CREATE TABLE IF NOT EXISTS download_tokens (
  token TEXT PRIMARY KEY,
  user_id TEXT NOT NULL REFERENCES users(id),
  r2_key TEXT NOT NULL,
  file_name TEXT NOT NULL,
  expires_at INTEGER NOT NULL
);
