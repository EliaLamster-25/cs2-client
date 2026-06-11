import { Hono } from 'hono';
import type { Env } from '../types';
import {
  bindHwid,
  ensureSubscription,
  getSubscription,
  getUserById,
  getUserByUsername,
  registerUser,
  renewFreeSubscription,
  subscriptionActive,
  subscriptionHardActive,
  verifyPassword,
  hashPasswordPlain,
} from '../lib/db';
import { hashRefreshToken, newRefreshToken, signJwt, verifyJwt } from '../lib/jwt';
import { addDays, error, isValidEmail, isValidUsername, json, normalizeUsername, nowSec, ok, parseJsonBody, sha256Hex } from '../lib/util';

type Ctx = { Bindings: Env; Variables: { userId: string; username: string } };

const auth = new Hono<Ctx>();

import { avatarUrlForUser } from './profile';
import { deriveSessionKey } from '../lib/session_crypto';

function sessionPayload(
  userId: string,
  username: string,
  plan: string,
  subExp: number,
  avatarUrl: string | null,
  payloadKey: string | null,
) {
  return {
    access_token: '',
    refresh_token: '',
    expires_in: 3600,
    token_type: 'Bearer',
    username,
    plan,
    subscription_expires_at: new Date(subExp * 1000).toISOString(),
    subscription_expires_unix: subExp,
    subscription_days_remaining: Math.max(0, Math.ceil((subExp - nowSec()) / 86400)),
    avatar_url: avatarUrl,
    payload_key: payloadKey,
    features: plan === 'beta' ? ['esp', 'aim', 'beta'] : ['esp', 'aim'],
  };
}

async function issueTokens(
  c: { env: Env; req: { url: string } },
  userId: string,
  username: string,
  plan: string,
  subExp: number,
  avatarKey: string | null,
  hwid?: string,
) {
  const claims = { sub: userId, username, plan, sub_exp: subExp };
  const access = await signJwt(claims, c.env.JWT_SECRET, 3600);
  const refresh = newRefreshToken();
  const refreshHash = await hashRefreshToken(refresh);
  const refreshExp = addDays(nowSec(), 30);
  await c.env.DB.prepare(
    'INSERT INTO refresh_tokens (token_hash, user_id, hwid, expires_at, created_at) VALUES (?, ?, ?, ?, ?)',
  )
    .bind(refreshHash, userId, hwid ?? null, refreshExp, nowSec())
    .run();
  const avatarUrl = avatarUrlForUser(c, userId, avatarKey);
  const secret = c.env.PAYLOAD_KEY?.trim();
  const payloadKey =
    secret && hwid !== undefined
      ? await deriveSessionKey(secret, access, hwid ?? '')
      : secret
        ? await deriveSessionKey(secret, access, '')
        : null;
  const body = sessionPayload(userId, username, plan, subExp, avatarUrl, payloadKey);
  body.access_token = access;
  body.refresh_token = refresh;
  return body;
}

auth.post('/register', async (c) => {
  const body = parseJsonBody<{
    invite_code?: string;
    email?: string;
    username?: string;
    password?: string;
    password_hash?: string;
  }>(await c.req.json().catch(() => null));
  if (!body?.invite_code || !body.email || !body.username) {
    return error('invite_code, email, and username are required.');
  }
  const username = normalizeUsername(body.username);
  if (!isValidUsername(username)) {
    return error('Username must be 3–32 chars: lowercase letters, numbers, underscore.');
  }
  if (!isValidEmail(body.email)) return error('Invalid email.');

  let passwordHash = body.password_hash ?? '';
  if (!passwordHash) {
    if (!body.password || body.password.length < 8) {
      return error('Password must be at least 8 characters.');
    }
    passwordHash = await hashPasswordPlain(body.password);
  }

  try {
    const user = await registerUser(c.env, {
      invite_code: body.invite_code,
      email: body.email,
      username,
      password_hash: passwordHash,
    });
    const sub = await ensureSubscription(c.env.DB, user.id);
    const tokens = await issueTokens(c, user.id, user.username, sub.plan, sub.expires_at, user.avatar_key ?? null);
    return ok(tokens);
  } catch (e) {
    return error(e instanceof Error ? e.message : 'Registration failed.', 400);
  }
});

auth.post('/login', async (c) => {
  const body = parseJsonBody<{
    username?: string;
    password?: string;
    password_hash?: string;
    hwid?: string;
  }>(await c.req.json().catch(() => null));
  if (!body?.username) return error('username is required.');
  const username = normalizeUsername(body.username);
  let passwordHash = body.password_hash ?? '';
  if (!passwordHash) {
    if (!body.password) return error('password or password_hash required.');
    passwordHash = await sha256Hex(body.password);
  }

  const user = await getUserByUsername(c.env.DB, username);
  if (!user || user.banned) return error('Invalid credentials.', 401);
  if (!(await verifyPassword(user, passwordHash))) return error('Invalid credentials.', 401);

  const sub = await ensureSubscription(c.env.DB, user.id);
  if (!subscriptionActive(sub)) {
    return error('Subscription expired. Renew at crymore.pw/dashboard', 403);
  }

  if (body.hwid) {
    const hw = await bindHwid(c.env.DB, user.id, sub.plan, body.hwid);
    if (!hw.ok) return error(hw.error, 403);
  }

  const tokens = await issueTokens(
    c,
    user.id,
    user.username,
    sub.plan,
    sub.expires_at,
    user.avatar_key ?? null,
    body.hwid,
  );
  if (!subscriptionHardActive(sub)) {
    (tokens as Record<string, unknown>).warning =
      'Subscription in grace period — renew at crymore.pw/dashboard';
  }
  return ok(tokens);
});

auth.post('/validate', async (c) => {
  const authHeader = c.req.header('authorization') ?? '';
  let token = authHeader.startsWith('Bearer ') ? authHeader.slice(7) : '';
  const body = parseJsonBody<{ access_token?: string; hwid?: string }>(
    await c.req.json().catch(() => ({})),
  );
  if (!token && body?.access_token) token = body.access_token;
  if (!token) return error('Missing access token.', 401);

  const claims = await verifyJwt(token, c.env.JWT_SECRET);
  if (!claims) return error('Invalid or expired token.', 401);

  const user = await getUserById(c.env.DB, claims.sub);
  if (!user || user.banned) return error('Account disabled.', 403);

  const sub = await getSubscription(c.env.DB, user.id);
  if (!subscriptionActive(sub)) {
    return error('Subscription expired. Renew at crymore.pw/dashboard', 403);
  }

  if (body?.hwid) {
    const hw = await bindHwid(c.env.DB, user.id, sub!.plan, body.hwid);
    if (!hw.ok) return error(hw.error, 403);
  }

  const secret = c.env.PAYLOAD_KEY?.trim();
  const payloadKey = secret
    ? await deriveSessionKey(secret, token, body?.hwid ?? '')
    : null;

  return ok({
    valid: true,
    username: user.username,
    plan: sub!.plan,
    subscription_expires_at: new Date(sub!.expires_at * 1000).toISOString(),
    subscription_expires_unix: sub!.expires_at,
    subscription_days_remaining: Math.max(0, Math.ceil((sub!.expires_at - nowSec()) / 86400)),
    avatar_url: avatarUrlForUser(c, user.id, user.avatar_key ?? null),
    payload_key: payloadKey,
    features: sub!.plan === 'beta' ? ['esp', 'aim', 'beta'] : ['esp', 'aim'],
  });
});

auth.post('/refresh', async (c) => {
  const body = parseJsonBody<{ refresh_token?: string; hwid?: string }>(
    await c.req.json().catch(() => null),
  );
  if (!body?.refresh_token) return error('refresh_token required.');
  const hash = await hashRefreshToken(body.refresh_token);
  const row = await c.env.DB.prepare(
    'SELECT * FROM refresh_tokens WHERE token_hash = ? AND revoked = 0',
  )
    .bind(hash)
    .first<{ user_id: string; expires_at: number; hwid: string | null }>();
  if (!row || row.expires_at < nowSec()) return error('Invalid refresh token.', 401);

  const user = await getUserById(c.env.DB, row.user_id);
  if (!user || user.banned) return error('Account disabled.', 403);
  const sub = await getSubscription(c.env.DB, user.id);
  if (!subscriptionActive(sub)) return error('Subscription expired.', 403);

  if (body.hwid) {
    const hw = await bindHwid(c.env.DB, user.id, sub!.plan, body.hwid);
    if (!hw.ok) return error(hw.error, 403);
  }

  await c.env.DB.prepare('UPDATE refresh_tokens SET revoked = 1 WHERE token_hash = ?')
    .bind(hash)
    .run();

  const tokens = await issueTokens(
    c,
    user.id,
    user.username,
    sub!.plan,
    sub!.expires_at,
    user.avatar_key ?? null,
    body.hwid ?? row.hwid ?? undefined,
  );
  return ok(tokens);
});

export { auth };
