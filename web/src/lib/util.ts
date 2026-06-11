export function uid(): string {
  return crypto.randomUUID();
}

export async function sha256Hex(input: string): Promise<string> {
  const data = new TextEncoder().encode(input);
  const hash = await crypto.subtle.digest('SHA-256', data);
  return [...new Uint8Array(hash)]
    .map((b) => b.toString(16).padStart(2, '0'))
    .join('');
}

export function nowSec(): number {
  return Math.floor(Date.now() / 1000);
}

export function addDays(sec: number, days: number): number {
  return sec + days * 86400;
}

export function json(data: unknown, status = 200): Response {
  return new Response(JSON.stringify(data), {
    status,
    headers: {
      'content-type': 'application/json; charset=utf-8',
      'cache-control': 'no-store',
    },
  });
}

export function error(message: string, status = 400): Response {
  return json({ ok: false, error: message }, status);
}

export function ok(data: Record<string, unknown> = {}): Response {
  return json({ ok: true, ...data });
}

export function normalizeUsername(u: string): string {
  return u.trim().toLowerCase();
}

export function isValidUsername(u: string): boolean {
  return /^[a-z0-9_]{3,32}$/.test(u);
}

export function isValidEmail(e: string): boolean {
  return /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(e);
}

export function parseJsonBody<T extends Record<string, unknown>>(body: unknown): T | null {
  if (!body || typeof body !== 'object') return null;
  return body as T;
}

export function corsHeaders(origin: string | null): HeadersInit {
  const allow = origin ?? '*';
  return {
    'access-control-allow-origin': allow,
    'access-control-allow-methods': 'GET, POST, OPTIONS',
    'access-control-allow-headers': 'content-type, authorization, x-admin-secret',
    'access-control-max-age': '86400',
  };
}
