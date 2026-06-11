import { Hono } from 'hono';
import type { Env } from '../types';
import { verifyJwt } from '../lib/jwt';
import { getUserById, updateUserPassword, verifyPassword } from '../lib/db';
import { error, ok, parseJsonBody, sha256Hex } from '../lib/util';
import { userFromBearer } from './subscription';

type Ctx = { Bindings: Env };

const profile = new Hono<Ctx>();

function avatarPublicUrl(c: { req: { url: string }; env: Env }, userId: string): string {
  const base = c.env.SITE_URL?.replace(/\/$/, '') ?? new URL(c.req.url).origin;
  return `${base}/v1/profile/avatar/${userId}`;
}

export function avatarUrlForUser(c: { req: { url: string }; env: Env }, userId: string, avatarKey: string | null): string | null {
  if (!avatarKey) return null;
  return avatarPublicUrl(c, userId);
}

profile.get('/avatar/:userId', async (c) => {
  const userId = c.req.param('userId');
  const user = await getUserById(c.env.DB, userId);
  if (!user?.avatar_key) {
    return c.env.ASSETS.fetch(new URL('/assets/picture_placeholder.png', c.req.url).toString());
  }
  const obj = await c.env.LOADER_BUCKET.get(user.avatar_key);
  if (!obj) {
    return c.env.ASSETS.fetch(new URL('/assets/picture_placeholder.png', c.req.url).toString());
  }
  const headers = new Headers();
  headers.set('content-type', obj.httpMetadata?.contentType ?? 'image/png');
  headers.set('cache-control', 'public, max-age=300');
  return new Response(obj.body, { headers });
});

profile.post('/avatar', async (c) => {
  const session = await userFromBearer(c);
  if (!session) return error('Unauthorized.', 401);

  const form = await c.req.formData().catch(() => null);
  if (!form) return error('Expected multipart form with avatar file.');

  const raw = form.get('avatar');
  if (!raw || typeof raw === 'string') return error('Missing avatar file.');
  const file = raw as File;

  if (file.size > 512 * 1024) return error('Avatar must be under 512 KB.');
  const type = file.type || 'application/octet-stream';
  if (!type.startsWith('image/')) return error('Avatar must be an image.');

  const ext = type.includes('jpeg') ? 'jpg' : type.includes('webp') ? 'webp' : 'png';
  const key = `avatars/${session.user.id}.${ext}`;
  const bytes = await file.arrayBuffer();

  await c.env.LOADER_BUCKET.put(key, bytes, {
    httpMetadata: { contentType: type },
  });

  await c.env.DB.prepare('UPDATE users SET avatar_key = ? WHERE id = ?')
    .bind(key, session.user.id)
    .run();

  return ok({
    avatar_url: avatarPublicUrl(c, session.user.id),
  });
});

profile.delete('/avatar', async (c) => {
  const session = await userFromBearer(c);
  if (!session) return error('Unauthorized.', 401);

  const user = await getUserById(c.env.DB, session.user.id);
  if (user?.avatar_key) {
    await c.env.LOADER_BUCKET.delete(user.avatar_key).catch(() => null);
  }
  await c.env.DB.prepare('UPDATE users SET avatar_key = NULL WHERE id = ?')
    .bind(session.user.id)
    .run();

  return ok({ avatar_url: null });
});

profile.get('/me', async (c) => {
  const session = await userFromBearer(c);
  if (!session) return error('Unauthorized.', 401);
  return ok({
    username: session.user.username,
    email: session.user.email,
    avatar_url: avatarUrlForUser(c, session.user.id, session.user.avatar_key ?? null),
  });
});

profile.post('/password', async (c) => {
  const session = await userFromBearer(c);
  if (!session) return error('Unauthorized.', 401);

  const body = parseJsonBody<{
    current_password?: string;
    current_password_hash?: string;
    new_password?: string;
    new_password_hash?: string;
  }>(await c.req.json().catch(() => null));

  let currentHash = body?.current_password_hash ?? '';
  if (!currentHash) {
    if (!body?.current_password) return error('Current password is required.');
    currentHash = await sha256Hex(body.current_password);
  }

  if (!(await verifyPassword(session.user, currentHash))) {
    return error('Current password is incorrect.', 403);
  }

  let newHash = body?.new_password_hash ?? '';
  if (!newHash) {
    if (!body?.new_password || body.new_password.length < 8) {
      return error('New password must be at least 8 characters.');
    }
    newHash = await sha256Hex(body.new_password);
  }

  if (newHash === currentHash) {
    return error('New password must be different from the current password.');
  }

  await updateUserPassword(c.env.DB, session.user.id, newHash);
  await c.env.DB.prepare('UPDATE refresh_tokens SET revoked = 1 WHERE user_id = ?')
    .bind(session.user.id)
    .run();

  return ok({ message: 'Password updated. Sign in again on other devices.' });
});

export { profile };
