import { Hono } from 'hono';
import type { Env } from '../types';
import { inviteCode } from '../lib/jwt';
import { error, nowSec, ok, uid } from '../lib/util';

type Ctx = { Bindings: Env };

function adminGuard(c: { env: Env; req: { header: (n: string) => string | undefined } }) {
  const secret = c.req.header('x-admin-secret') ?? '';
  if (!c.env.ADMIN_SECRET || secret !== c.env.ADMIN_SECRET) return false;
  return true;
}

const admin = new Hono<Ctx>();

admin.use('*', async (c, next) => {
  if (!adminGuard(c)) return error('Forbidden.', 403);
  await next();
});

admin.post('/invites', async (c) => {
  const body = (await c.req.json().catch(() => ({}))) as {
    max_uses?: number;
    expires_days?: number;
    note?: string;
  };
  const code = inviteCode();
  const maxUses = Math.max(1, Math.min(body.max_uses ?? 1, 1000));
  const expiresAt = body.expires_days ? nowSec() + body.expires_days * 86400 : null;
  await c.env.DB.prepare(
    'INSERT INTO invite_codes (code, max_uses, uses, expires_at, note, created_at) VALUES (?, ?, 0, ?, ?, ?)',
  )
    .bind(code, maxUses, expiresAt, body.note ?? null, nowSec())
    .run();
  return ok({ code, max_uses: maxUses, expires_at: expiresAt });
});

admin.get('/invites', async (c) => {
  const rows = await c.env.DB.prepare(
    'SELECT code, max_uses, uses, expires_at, note, created_at FROM invite_codes ORDER BY created_at DESC LIMIT 100',
  ).all();
  return ok({ invites: rows.results ?? [] });
});

admin.post('/releases', async (c) => {
  const body = (await c.req.json().catch(() => ({}))) as {
    version?: string;
    channel?: string;
    r2_key?: string;
    file_name?: string;
    overlay_r2_key?: string;
    overlay_dek_b64?: string;
  };
  if (!body.version || !body.r2_key) {
    return error('version and r2_key required (upload zip to R2 first).');
  }
  const id = uid();
  await c.env.DB.prepare(
    'INSERT INTO releases (id, version, channel, r2_key, file_name, overlay_r2_key, overlay_dek_b64, published_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)',
  )
    .bind(
      id,
      body.version,
      body.channel ?? 'stable',
      body.r2_key,
      body.file_name ?? 'crymore-loader.zip',
      body.overlay_r2_key ?? null,
      body.overlay_dek_b64 ?? null,
      nowSec(),
    )
    .run();
  return ok({
    id,
    version: body.version,
    channel: body.channel ?? 'stable',
    overlay: Boolean(body.overlay_r2_key),
  });
});

admin.get('/stats', async (c) => {
  const users = await c.env.DB.prepare('SELECT COUNT(*) as c FROM users').first<{ c: number }>();
  const active = await c.env.DB.prepare(
    'SELECT COUNT(*) as c FROM subscriptions WHERE expires_at > ?',
  )
    .bind(nowSec())
    .first<{ c: number }>();
  const invites = await c.env.DB.prepare('SELECT COUNT(*) as c FROM invite_codes').first<{ c: number }>();
  return ok({
    users: users?.c ?? 0,
    active_subscriptions: active?.c ?? 0,
    invite_codes: invites?.c ?? 0,
  });
});

admin.post('/bootstrap', async (c) => {
  // One-time: create first invite if none exist (still requires admin secret).
  const count = await c.env.DB.prepare('SELECT COUNT(*) as c FROM invite_codes').first<{ c: number }>();
  if ((count?.c ?? 0) > 0) return error('Invites already exist.');
  const code = inviteCode();
  await c.env.DB.prepare(
    'INSERT INTO invite_codes (code, max_uses, uses, expires_at, note, created_at) VALUES (?, 5, 0, NULL, ?, ?)',
  )
    .bind(code, 'bootstrap', nowSec())
    .run();
  return ok({ code, message: 'First invite created.' });
});

export { admin };
