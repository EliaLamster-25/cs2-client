import type { JwtClaims } from '../types';
import { nowSec } from './util';

function b64url(input: ArrayBuffer | Uint8Array | string): string {
  let bytes: Uint8Array;
  if (typeof input === 'string') {
    bytes = new TextEncoder().encode(input);
  } else if (input instanceof ArrayBuffer) {
    bytes = new Uint8Array(input);
  } else {
    bytes = input;
  }
  let bin = '';
  for (const b of bytes) bin += String.fromCharCode(b);
  return btoa(bin).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}

function fromB64url(s: string): Uint8Array {
  const pad = s.length % 4 === 0 ? '' : '='.repeat(4 - (s.length % 4));
  const b64 = s.replace(/-/g, '+').replace(/_/g, '/') + pad;
  const bin = atob(b64);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; ++i) out[i] = bin.charCodeAt(i);
  return out;
}

async function hmacKey(secret: string): Promise<CryptoKey> {
  return crypto.subtle.importKey(
    'raw',
    new TextEncoder().encode(secret),
    { name: 'HMAC', hash: 'SHA-256' },
    false,
    ['sign', 'verify'],
  );
}

export async function signJwt(
  claims: JwtClaims,
  secret: string,
  ttlSec: number,
): Promise<string> {
  const header = { alg: 'HS256', typ: 'JWT' };
  const now = nowSec();
  const payload = { ...claims, iat: now, exp: now + ttlSec };
  const head = b64url(JSON.stringify(header));
  const body = b64url(JSON.stringify(payload));
  const data = `${head}.${body}`;
  const key = await hmacKey(secret);
  const sig = await crypto.subtle.sign('HMAC', key, new TextEncoder().encode(data));
  return `${data}.${b64url(sig)}`;
}

export async function verifyJwt(token: string, secret: string): Promise<JwtClaims | null> {
  const parts = token.split('.');
  if (parts.length !== 3) return null;
  const [head, body, sig] = parts;
  const data = `${head}.${body}`;
  const key = await hmacKey(secret);
  const valid = await crypto.subtle.verify(
    'HMAC',
    key,
    fromB64url(sig),
    new TextEncoder().encode(data),
  );
  if (!valid) return null;
  try {
    const payload = JSON.parse(new TextDecoder().decode(fromB64url(body))) as JwtClaims & {
      exp?: number;
    };
    if (!payload.sub || !payload.username || !payload.exp || payload.exp < nowSec()) return null;
    return {
      sub: payload.sub,
      username: payload.username,
      plan: payload.plan ?? 'free',
      sub_exp: payload.sub_exp ?? 0,
    };
  } catch {
    return null;
  }
}

export async function hashRefreshToken(token: string): Promise<string> {
  const data = new TextEncoder().encode(token);
  const hash = await crypto.subtle.digest('SHA-256', data);
  return [...new Uint8Array(hash)].map((b) => b.toString(16).padStart(2, '0')).join('');
}

export function newRefreshToken(): string {
  const bytes = new Uint8Array(32);
  crypto.getRandomValues(bytes);
  return [...bytes].map((b) => b.toString(16).padStart(2, '0')).join('');
}

export function inviteCode(): string {
  const alphabet = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
  const bytes = new Uint8Array(10);
  crypto.getRandomValues(bytes);
  let out = '';
  for (const b of bytes) out += alphabet[b % alphabet.length];
  return `CRY-${out.slice(0, 5)}-${out.slice(5)}`;
}
