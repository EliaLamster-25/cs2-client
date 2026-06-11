import type { Env, SubscriptionRow, UserRow } from '../types';
import { addDays, nowSec, sha256Hex, uid } from './util';

const FREE_DAYS = 30;
const GRACE_DAYS = 3;
const MAX_HWID_FREE = 1;
const MAX_HWID_BETA = 2;

export function subscriptionActive(sub: SubscriptionRow | null): boolean {
  if (!sub) return false;
  const grace = addDays(sub.expires_at, GRACE_DAYS);
  return nowSec() <= grace;
}

export function subscriptionHardActive(sub: SubscriptionRow | null): boolean {
  if (!sub) return false;
  return nowSec() <= sub.expires_at;
}

export async function getUserByUsername(db: D1Database, username: string): Promise<UserRow | null> {
  return db
    .prepare('SELECT * FROM users WHERE username = ? COLLATE NOCASE')
    .bind(username)
    .first<UserRow>();
}

export async function getUserById(db: D1Database, id: string): Promise<UserRow | null> {
  return db.prepare('SELECT * FROM users WHERE id = ?').bind(id).first<UserRow>();
}

export async function getSubscription(db: D1Database, userId: string): Promise<SubscriptionRow | null> {
  return db
    .prepare('SELECT * FROM subscriptions WHERE user_id = ?')
    .bind(userId)
    .first<SubscriptionRow>();
}

export async function ensureSubscription(db: D1Database, userId: string): Promise<SubscriptionRow> {
  const existing = await getSubscription(db, userId);
  if (existing) return existing;
  const exp = addDays(nowSec(), FREE_DAYS);
  await db
    .prepare(
      'INSERT INTO subscriptions (user_id, plan, expires_at, renewed_at) VALUES (?, ?, ?, ?)',
    )
    .bind(userId, 'free', exp, nowSec())
    .run();
  return (await getSubscription(db, userId))!;
}

export async function renewFreeSubscription(db: D1Database, userId: string): Promise<SubscriptionRow> {
  const sub = await ensureSubscription(db, userId);
  const now = nowSec();
  // Allow renew if within 5 days of expiry or already expired (in grace handled separately)
  const earliest = sub.expires_at - 5 * 86400;
  if (now < earliest && subscriptionHardActive(sub)) {
    throw new Error('Renewal opens 5 days before expiry.');
  }
  const base = Math.max(now, sub.expires_at);
  const newExp = addDays(base, FREE_DAYS);
  await db
    .prepare('UPDATE subscriptions SET expires_at = ?, renewed_at = ?, plan = ? WHERE user_id = ?')
    .bind(newExp, now, sub.plan === 'beta' ? 'beta' : 'free', userId)
    .run();
  return (await getSubscription(db, userId))!;
}

export async function consumeInvite(
  db: D1Database,
  code: string,
): Promise<{ ok: true } | { ok: false; error: string }> {
  const row = await db
    .prepare('SELECT * FROM invite_codes WHERE code = ?')
    .bind(code.trim().toUpperCase())
    .first<{ code: string; max_uses: number; uses: number; expires_at: number | null }>();
  if (!row) return { ok: false, error: 'Invalid invite code.' };
  if (row.expires_at && row.expires_at < nowSec()) return { ok: false, error: 'Invite code expired.' };
  if (row.uses >= row.max_uses) return { ok: false, error: 'Invite code already used.' };
  await db
    .prepare('UPDATE invite_codes SET uses = uses + 1 WHERE code = ?')
    .bind(row.code)
    .run();
  return { ok: true };
}

export async function registerUser(
  env: Env,
  input: {
    invite_code: string;
    email: string;
    username: string;
    password_hash: string;
  },
): Promise<UserRow> {
  const invite = await consumeInvite(env.DB, input.invite_code);
  if (!invite.ok) throw new Error(invite.error);

  const id = uid();
  const ts = nowSec();
  try {
    await env.DB.prepare(
      'INSERT INTO users (id, email, username, password_hash, created_at) VALUES (?, ?, ?, ?, ?)',
    )
      .bind(id, input.email.trim().toLowerCase(), input.username.trim().toLowerCase(), input.password_hash, ts)
      .run();
  } catch {
    throw new Error('Email or username already taken.');
  }
  await ensureSubscription(env.DB, id);
  return (await getUserById(env.DB, id))!;
}

export async function verifyPassword(user: UserRow, passwordHash: string): Promise<boolean> {
  return user.password_hash === passwordHash;
}

export async function hashPasswordPlain(plain: string): Promise<string> {
  return sha256Hex(plain);
}

export async function updateUserPassword(db: D1Database, userId: string, passwordHash: string): Promise<void> {
  await db.prepare('UPDATE users SET password_hash = ? WHERE id = ?')
    .bind(passwordHash, userId)
    .run();
}

export async function bindHwid(
  db: D1Database,
  userId: string,
  plan: string,
  hwid: string,
): Promise<{ ok: true } | { ok: false; error: string }> {
  if (!hwid || hwid === 'hwid_unavailable') {
    return { ok: false, error: 'Could not read hardware ID from this PC.' };
  }
  const max = plan === 'beta' ? MAX_HWID_BETA : MAX_HWID_FREE;
  const existing = await db
    .prepare('SELECT hwid FROM hwid_bindings WHERE user_id = ? AND hwid = ?')
    .bind(userId, hwid)
    .first();
  const now = nowSec();
  if (existing) {
    await db
      .prepare('UPDATE hwid_bindings SET last_seen = ? WHERE user_id = ? AND hwid = ?')
      .bind(now, userId, hwid)
      .run();
    return { ok: true };
  }
  const countRow = await db
    .prepare('SELECT COUNT(*) as c FROM hwid_bindings WHERE user_id = ?')
    .bind(userId)
    .first<{ c: number }>();
  const count = countRow?.c ?? 0;
  if (count >= max) {
    return {
      ok: false,
      error: `HWID limit reached (${max} device${max > 1 ? 's' : ''}). Reset via dashboard or support.`,
    };
  }
  await db
    .prepare(
      'INSERT INTO hwid_bindings (user_id, hwid, first_seen, last_seen) VALUES (?, ?, ?, ?)',
    )
    .bind(userId, hwid, now, now)
    .run();
  return { ok: true };
}

export async function resetHwids(db: D1Database, userId: string): Promise<void> {
  await db.prepare('DELETE FROM hwid_bindings WHERE user_id = ?').bind(userId).run();
}

export { FREE_DAYS, GRACE_DAYS };
