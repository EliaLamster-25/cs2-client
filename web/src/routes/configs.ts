import { Hono } from 'hono';
import type { Env } from '../types';
import { error, ok, parseJsonBody, nowSec } from '../lib/util';
import { userFromBearer } from './subscription';
import { subscriptionActive, ensureSubscription } from '../lib/db';

type Ctx = { Bindings: Env };

const configs = new Hono<Ctx>();

async function requireActiveSub(c: { env: Env; req: { header: (n: string) => string | undefined } }) {
  const session = await userFromBearer(c);
  if (!session) return null;
  const sub = await ensureSubscription(c.env.DB, session.user.id);
  if (!subscriptionActive(sub)) return null;
  return session;
}

configs.get('/', async (c) => {
  const session = await requireActiveSub(c);
  if (!session) return error('Unauthorized or subscription inactive.', 401);

  const rows = await c.env.DB.prepare(
    `SELECT id, name, description, is_public, updated_at, created_at
     FROM user_configs WHERE user_id = ? ORDER BY updated_at DESC`,
  )
    .bind(session.user.id)
    .all<{
      id: string;
      name: string;
      description: string;
      is_public: number;
      updated_at: number;
      created_at: number;
    }>();

  return ok({
    configs: (rows.results ?? []).map((r) => ({
      id: r.id,
      name: r.name,
      description: r.description,
      is_public: r.is_public === 1,
      updated_at: new Date(r.updated_at * 1000).toISOString(),
      created_at: new Date(r.created_at * 1000).toISOString(),
    })),
  });
});

configs.get('/:id', async (c) => {
  const session = await requireActiveSub(c);
  if (!session) return error('Unauthorized or subscription inactive.', 401);

  const id = c.req.param('id');
  const row = await c.env.DB.prepare(
    'SELECT id, name, description, r2_key, is_public, user_id FROM user_configs WHERE id = ?',
  )
    .bind(id)
    .first<{ id: string; name: string; description: string; r2_key: string; is_public: number; user_id: string }>();

  if (!row) return error('Config not found.', 404);
  if (row.user_id !== session.user.id && row.is_public !== 1)
    return error('Forbidden.', 403);

  const obj = await c.env.LOADER_BUCKET.get(row.r2_key);
  if (!obj) return error('Config data missing.', 404);

  const text = await obj.text();
  return ok({
    id: row.id,
    name: row.name,
    description: row.description,
    is_public: row.is_public === 1,
    json: text,
  });
});

configs.post('/', async (c) => {
  const session = await requireActiveSub(c);
  if (!session) return error('Unauthorized or subscription inactive.', 401);

  const body = await parseJsonBody<{ name?: string; description?: string; json?: string; is_public?: boolean }>(c);
  if (!body?.name?.trim() || !body?.json?.trim())
    return error('name and json are required.');

  const id = crypto.randomUUID();
  const ts = nowSec();
  const r2Key = `configs/${session.user.id}/${id}.json`;

  await c.env.LOADER_BUCKET.put(r2Key, body.json, {
    httpMetadata: { contentType: 'application/json' },
  });

  await c.env.DB.prepare(
    `INSERT INTO user_configs (id, user_id, name, description, r2_key, is_public, updated_at, created_at)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
  )
    .bind(
      id,
      session.user.id,
      body.name.trim().slice(0, 64),
      (body.description ?? '').slice(0, 256),
      r2Key,
      body.is_public ? 1 : 0,
      ts,
      ts,
    )
    .run();

  return ok({ id, name: body.name.trim() });
});

configs.put('/:id', async (c) => {
  const session = await requireActiveSub(c);
  if (!session) return error('Unauthorized or subscription inactive.', 401);

  const id = c.req.param('id');
  const row = await c.env.DB.prepare(
    'SELECT id, r2_key FROM user_configs WHERE id = ? AND user_id = ?',
  )
    .bind(id, session.user.id)
    .first<{ id: string; r2_key: string }>();
  if (!row) return error('Config not found.', 404);

  const body = await parseJsonBody<{ name?: string; description?: string; json?: string; is_public?: boolean }>(c);
  if (!body?.json?.trim()) return error('json is required.');

  await c.env.LOADER_BUCKET.put(row.r2_key, body.json, {
    httpMetadata: { contentType: 'application/json' },
  });

  const ts = nowSec();
  await c.env.DB.prepare(
    `UPDATE user_configs SET name = COALESCE(?, name), description = COALESCE(?, description),
     is_public = COALESCE(?, is_public), updated_at = ? WHERE id = ?`,
  )
    .bind(
      body.name?.trim().slice(0, 64) ?? null,
      body.description?.slice(0, 256) ?? null,
      body.is_public === undefined ? null : body.is_public ? 1 : 0,
      ts,
      id,
    )
    .run();

  return ok({ id, updated: true });
});

configs.delete('/:id', async (c) => {
  const session = await requireActiveSub(c);
  if (!session) return error('Unauthorized or subscription inactive.', 401);

  const id = c.req.param('id');
  const row = await c.env.DB.prepare(
    'SELECT r2_key FROM user_configs WHERE id = ? AND user_id = ?',
  )
    .bind(id, session.user.id)
    .first<{ r2_key: string }>();
  if (!row) return error('Config not found.', 404);

  await c.env.LOADER_BUCKET.delete(row.r2_key).catch(() => null);
  await c.env.DB.prepare('DELETE FROM user_configs WHERE id = ?').bind(id).run();
  return ok({ deleted: true });
});

export { configs };
