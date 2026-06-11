-- Cloud configs and grenade lineup packs

CREATE TABLE IF NOT EXISTS user_configs (
  id TEXT PRIMARY KEY,
  user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  name TEXT NOT NULL,
  description TEXT NOT NULL DEFAULT '',
  r2_key TEXT NOT NULL,
  is_public INTEGER NOT NULL DEFAULT 0,
  updated_at INTEGER NOT NULL,
  created_at INTEGER NOT NULL,
  UNIQUE(user_id, name)
);

CREATE INDEX IF NOT EXISTS idx_user_configs_user ON user_configs(user_id);

CREATE TABLE IF NOT EXISTS lineup_packs (
  id TEXT PRIMARY KEY,
  user_id TEXT REFERENCES users(id) ON DELETE SET NULL,
  map_name TEXT NOT NULL,
  title TEXT NOT NULL,
  description TEXT NOT NULL DEFAULT '',
  grenade_type TEXT NOT NULL DEFAULT 'any',
  r2_key TEXT NOT NULL,
  spot_count INTEGER NOT NULL DEFAULT 0,
  is_public INTEGER NOT NULL DEFAULT 1,
  download_count INTEGER NOT NULL DEFAULT 0,
  published_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_lineup_packs_map ON lineup_packs(map_name);
CREATE INDEX IF NOT EXISTS idx_lineup_packs_public ON lineup_packs(is_public);
