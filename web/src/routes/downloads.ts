import { Hono } from 'hono';

import type { Env } from '../types';

import { ensureSubscription, subscriptionActive } from '../lib/db';

import { error, ok, parseJsonBody } from '../lib/util';

import { userFromBearer } from './subscription';

import { deriveSessionKey, wrapDek } from '../lib/session_crypto';

import { r2PresignedGetUrl } from '../lib/r2_presign';

type Ctx = { Bindings: Env };

const downloads = new Hono<Ctx>();

function bearerToken(c: { req: { header: (n: string) => string | undefined } }): string {
  const h = c.req.header('authorization') ?? '';
  return h.startsWith('Bearer ') ? h.slice(7) : '';
}

downloads.get('/latest', async (c) => {
  const channel = c.req.query('channel') ?? 'stable';
  const row = await c.env.DB.prepare(
    'SELECT * FROM releases WHERE channel = ? ORDER BY published_at DESC LIMIT 1',
  )
    .bind(channel)
    .first<{ version: string; channel: string; file_name: string; published_at: number }>();
  if (!row) return error('No release published yet.', 404);
  return ok({
    version: row.version,
    channel: row.channel,
    file_name: row.file_name,
    published_at: new Date(row.published_at * 1000).toISOString(),
  });
});

downloads.post('/loader', async (c) => {
  const session = await userFromBearer(c);
  if (!session) return error('Unauthorized.', 401);
  const sub = await ensureSubscription(c.env.DB, session.user.id);
  if (!subscriptionActive(sub)) {
    return error('Active subscription required. Renew at dashboard.', 403);
  }

  const channel = sub.plan === 'beta' ? 'beta' : 'stable';
  const release = await c.env.DB.prepare(
    'SELECT * FROM releases WHERE channel = ? ORDER BY published_at DESC LIMIT 1',
  )
    .bind(channel)
    .first<{ r2_key: string; file_name: string; version: string }>();

  if (!release) {
    return error('Loader not published yet. Check back soon.', 503);
  }

  const obj = await c.env.LOADER_BUCKET.head(release.r2_key);
  if (!obj) {
    return error('Loader file missing on server. Contact support.', 503);
  }

  const token = crypto.randomUUID();
  const exp = Math.floor(Date.now() / 1000) + 900;
  await c.env.DB.prepare(
    'INSERT OR REPLACE INTO download_tokens (token, user_id, r2_key, file_name, expires_at) VALUES (?, ?, ?, ?, ?)',
  )
    .bind(token, session.user.id, release.r2_key, release.file_name, exp)
    .run()
    .catch(() => null);

  const presigned = await r2PresignedGetUrl(c.env, release.r2_key, 900, release.file_name);
  const base = new URL(c.req.url);
  const downloadUrl = presigned ?? `${base.origin}/v1/downloads/file/${token}`;

  return ok({
    version: release.version,
    file_name: release.file_name,
    download_url: downloadUrl,
    expires_in: 900,
    direct: Boolean(presigned),
  });
});

/** Session-bound overlay payload (encrypted on R2). Strongest path for Release builds. */
downloads.post('/overlay', async (c) => {
  const session = await userFromBearer(c);
  if (!session) return error('Unauthorized.', 401);

  const sub = await ensureSubscription(c.env.DB, session.user.id);
  if (!subscriptionActive(sub)) {
    return error('Active subscription required.', 403);
  }

  const secret = c.env.PAYLOAD_KEY?.trim();
  if (!secret) return error('Payload protection not configured on server.', 503);

  const body = parseJsonBody<{ hwid?: string }>(await c.req.json().catch(() => ({})));
  const hwid = body?.hwid ?? '';
  if (!hwid) return error('hwid required.', 400);

  const channel = sub.plan === 'beta' ? 'beta' : 'stable';
  const release = await c.env.DB.prepare(
    'SELECT * FROM releases WHERE channel = ? ORDER BY published_at DESC LIMIT 1',
  )
    .bind(channel)
    .first<{
      version: string;
      overlay_r2_key: string | null;
      overlay_dek_b64: string | null;
    }>();

  if (!release?.overlay_r2_key || !release.overlay_dek_b64) {
    return error('Overlay payload not published for this channel.', 503);
  }

  const head = await c.env.LOADER_BUCKET.head(release.overlay_r2_key);
  if (!head) return error('Overlay file missing on server.', 503);

  const accessToken = bearerToken(c);
  if (!accessToken) return error('Missing bearer token.', 401);

  const sessionKey = await deriveSessionKey(secret, accessToken, hwid);
  const dekWrap = await wrapDek(sessionKey, release.overlay_dek_b64);

  const token = crypto.randomUUID();
  const exp = Math.floor(Date.now() / 1000) + 600;
  await c.env.DB.prepare(
    'INSERT OR REPLACE INTO download_tokens (token, user_id, r2_key, file_name, expires_at) VALUES (?, ?, ?, ?, ?)',
  )
    .bind(token, session.user.id, release.overlay_r2_key, 'overlay.enc', exp)
    .run();

  const presigned = await r2PresignedGetUrl(c.env, release.overlay_r2_key, 600, 'overlay.enc');
  const base = new URL(c.req.url);
  const downloadUrl = presigned ?? `${base.origin}/v1/downloads/file/${token}`;

  return ok({
    version: release.version,
    download_url: downloadUrl,
    dek_wrap: dekWrap,
    expires_in: 600,
    direct: Boolean(presigned),
  });
});

/** Validates token, then redirects to R2 presigned URL (fast) or streams via Worker (slow fallback). */
downloads.get('/file/:token', async (c) => {
  const token = c.req.param('token');
  const row = await c.env.DB.prepare('SELECT * FROM download_tokens WHERE token = ?')
    .bind(token)
    .first<{ r2_key: string; file_name: string; expires_at: number }>();
  if (!row || row.expires_at < Math.floor(Date.now() / 1000)) {
    return error('Download link expired or invalid.', 404);
  }

  const ttl = Math.max(60, row.expires_at - Math.floor(Date.now() / 1000));
  const presigned = await r2PresignedGetUrl(c.env, row.r2_key, ttl, row.file_name);
  if (presigned) {
    await c.env.DB.prepare('DELETE FROM download_tokens WHERE token = ?').bind(token).run();
    return Response.redirect(presigned, 302);
  }

  const obj = await c.env.LOADER_BUCKET.get(row.r2_key);
  if (!obj) return error('File not found.', 404);

  await c.env.DB.prepare('DELETE FROM download_tokens WHERE token = ?').bind(token).run();

  const headers = new Headers();
  const isOverlay = row.file_name === 'overlay.enc';
  headers.set('content-type', isOverlay ? 'application/octet-stream' : 'application/zip');
  headers.set(
    'content-disposition',
    `attachment; filename="${row.file_name.replace(/"/g, '')}"`,
  );
  headers.set('cache-control', 'no-store');
  if (obj.size) headers.set('content-length', String(obj.size));

  return new Response(obj.body, { headers });
});

export { downloads };
