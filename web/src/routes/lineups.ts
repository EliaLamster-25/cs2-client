import { Hono } from 'hono';
import type { Env } from '../types';
import { error, ok, parseJsonBody, nowSec } from '../lib/util';
import { userFromBearer } from './subscription';
import { subscriptionActive, ensureSubscription } from '../lib/db';

type Ctx = { Bindings: Env };

const lineups = new Hono<Ctx>();

async function requireActiveSub(c: { env: Env; req: { header: (n: string) => string | undefined } }) {
  const session = await userFromBearer(c);
  if (!session) return null;
  const sub = await ensureSubscription(c.env.DB, session.user.id);
  if (!subscriptionActive(sub)) return null;
  return session;
}

lineups.get('/', async (c) => {
  const map = c.req.query('map')?.trim() ?? '';
  let query = `SELECT id, map_name, title, description, grenade_type, spot_count, download_count, published_at
               FROM lineup_packs WHERE is_public = 1`;
  const binds: string[] = [];
  if (map) {
    query += ' AND map_name = ?';
    binds.push(map);
  }
  query += ' ORDER BY published_at DESC LIMIT 100';

  const stmt = c.env.DB.prepare(query);
  const rows = binds.length
    ? await stmt.bind(...binds).all<{
        id: string;
        map_name: string;
        title: string;
        description: string;
        grenade_type: string;
        spot_count: number;
        download_count: number;
        published_at: number;
      }>()
    : await stmt.all<{
        id: string;
        map_name: string;
        title: string;
        description: string;
        grenade_type: string;
        spot_count: number;
        download_count: number;
        published_at: number;
      }>();

  return ok({
    packs: (rows.results ?? []).map((r) => ({
      id: r.id,
      map: r.map_name,
      title: r.title,
      description: r.description,
      grenade_type: r.grenade_type,
      spot_count: r.spot_count,
      download_count: r.download_count,
      published_at: new Date(r.published_at * 1000).toISOString(),
    })),
  });
});

lineups.get('/:id', async (c) => {
  const id = c.req.param('id');
  const row = await c.env.DB.prepare(
    'SELECT id, map_name, title, description, grenade_type, r2_key, spot_count, is_public FROM lineup_packs WHERE id = ?',
  )
    .bind(id)
    .first<{
      id: string;
      map_name: string;
      title: string;
      description: string;
      grenade_type: string;
      r2_key: string;
      spot_count: number;
      is_public: number;
    }>();

  if (!row) return error('Lineup pack not found.', 404);

  const session = await userFromBearer(c);
  if (row.is_public !== 1) {
    if (!session) return error('Unauthorized.', 401);
    const owner = await c.env.DB.prepare('SELECT user_id FROM lineup_packs WHERE id = ?')
      .bind(id)
      .first<{ user_id: string | null }>();
    if (owner?.user_id !== session.user.id) return error('Forbidden.', 403);
  }

  const obj = await c.env.LOADER_BUCKET.get(row.r2_key);
  if (!obj) return error('Pack data missing.', 404);

  await c.env.DB.prepare('UPDATE lineup_packs SET download_count = download_count + 1 WHERE id = ?')
    .bind(id)
    .run();

  const json = await obj.text();
  return ok({
    id: row.id,
    map: row.map_name,
    title: row.title,
    description: row.description,
    grenade_type: row.grenade_type,
    spot_count: row.spot_count,
    json,
  });
});

lineups.post('/', async (c) => {
  const session = await requireActiveSub(c);
  if (!session) return error('Unauthorized or subscription inactive.', 401);

  const body = await parseJsonBody<{
    title?: string;
    map?: string;
    description?: string;
    grenade_type?: string;
    json?: string;
    is_public?: boolean;
  }>(c);

  if (!body?.title?.trim() || !body?.map?.trim() || !body?.json?.trim())
    return error('title, map, and json are required.');

  let spotCount = 0;
  try {
    const parsed = JSON.parse(body.json) as { spots?: unknown[] };
    spotCount = Array.isArray(parsed.spots) ? parsed.spots.length : 0;
  } catch {
    return error('Invalid lineup JSON.');
  }

  const id = crypto.randomUUID();
  const ts = nowSec();
  const r2Key = `lineups/${body.map.trim()}/${id}.json`;

  await c.env.LOADER_BUCKET.put(r2Key, body.json, {
    httpMetadata: { contentType: 'application/json' },
  });

  await c.env.DB.prepare(
    `INSERT INTO lineup_packs
     (id, user_id, map_name, title, description, grenade_type, r2_key, spot_count, is_public, download_count, published_at, updated_at)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?, ?)`,
  )
    .bind(
      id,
      session.user.id,
      body.map.trim().slice(0, 64),
      body.title.trim().slice(0, 128),
      (body.description ?? '').slice(0, 512),
      (body.grenade_type ?? 'any').slice(0, 32),
      r2Key,
      spotCount,
      body.is_public === false ? 0 : 1,
      ts,
      ts,
    )
    .run();

  return ok({ id, title: body.title.trim(), spot_count: spotCount });
});

lineups.delete('/:id', async (c) => {
  const session = await requireActiveSub(c);
  if (!session) return error('Unauthorized or subscription inactive.', 401);

  const id = c.req.param('id');
  const row = await c.env.DB.prepare(
    'SELECT r2_key FROM lineup_packs WHERE id = ? AND user_id = ?',
  )
    .bind(id, session.user.id)
    .first<{ r2_key: string }>();
  if (!row) return error('Pack not found.', 404);

  await c.env.LOADER_BUCKET.delete(row.r2_key).catch(() => null);
  await c.env.DB.prepare('DELETE FROM lineup_packs WHERE id = ?').bind(id).run();
  return ok({ deleted: true });
});

export { lineups };
