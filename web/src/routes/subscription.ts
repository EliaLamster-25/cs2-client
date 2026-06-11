import { Hono } from 'hono';
import type { Env } from '../types';
import { verifyJwt } from '../lib/jwt';
import {
  ensureSubscription,
  getSubscription,
  getUserById,
  renewFreeSubscription,
  resetHwids,
  subscriptionActive,
  subscriptionHardActive,
} from '../lib/db';
import { avatarUrlForUser } from './profile';
import { nowSec, ok, error } from '../lib/util';

type Ctx = { Bindings: Env };

async function userFromBearer(c: { env: Env; req: { header: (n: string) => string | undefined } }) {
  const authHeader = c.req.header('authorization') ?? '';
  const token = authHeader.startsWith('Bearer ') ? authHeader.slice(7) : '';
  if (!token) return null;
  const claims = await verifyJwt(token, c.env.JWT_SECRET);
  if (!claims) return null;
  const user = await getUserById(c.env.DB, claims.sub);
  if (!user || user.banned) return null;
  return { user, claims };
}

const sub = new Hono<Ctx>();

sub.get('/status', async (c) => {
  const session = await userFromBearer(c);
  if (!session) return error('Unauthorized.', 401);
  const row = await ensureSubscription(c.env.DB, session.user.id);
  const hwids = await c.env.DB.prepare(
    'SELECT hwid, first_seen, last_seen FROM hwid_bindings WHERE user_id = ?',
  )
    .bind(session.user.id)
    .all<{ hwid: string; first_seen: number; last_seen: number }>();

  return ok({
    username: session.user.username,
    email: session.user.email,
    plan: row.plan,
    expires_at: new Date(row.expires_at * 1000).toISOString(),
    subscription_days_remaining: Math.max(0, Math.ceil((row.expires_at - nowSec()) / 86400)),
    avatar_url: avatarUrlForUser(c, session.user.id, session.user.avatar_key ?? null),
    active: subscriptionActive(row),
    in_grace: !subscriptionHardActive(row) && subscriptionActive(row),
    renewed_at: row.renewed_at ? new Date(row.renewed_at * 1000).toISOString() : null,
    hwids: hwids.results ?? [],
  });
});

sub.post('/renew', async (c) => {
  const session = await userFromBearer(c);
  if (!session) return error('Unauthorized.', 401);
  try {
    const row = await renewFreeSubscription(c.env.DB, session.user.id);
    return ok({
      plan: row.plan,
      expires_at: new Date(row.expires_at * 1000).toISOString(),
      message: 'Free access renewed for 30 days.',
    });
  } catch (e) {
    return error(e instanceof Error ? e.message : 'Renewal failed.', 400);
  }
});

sub.post('/hwid/reset', async (c) => {
  const session = await userFromBearer(c);
  if (!session) return error('Unauthorized.', 401);
  const last = await c.env.DB.prepare(
    'SELECT MAX(last_seen) as t FROM hwid_bindings WHERE user_id = ?',
  )
    .bind(session.user.id)
    .first<{ t: number | null }>();
  if (last?.t && last.t > nowSec() - 30 * 86400) {
    return error('HWID reset available once every 30 days.', 429);
  }
  await resetHwids(c.env.DB, session.user.id);
  return ok({ message: 'Hardware bindings cleared. Log in again on your PC.' });
});

export { sub, userFromBearer };
