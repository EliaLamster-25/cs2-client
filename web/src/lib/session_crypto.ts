function enc(s: string): Uint8Array {
  return new TextEncoder().encode(s);
}

function b64(bytes: Uint8Array): string {
  let bin = '';
  for (const b of bytes) bin += String.fromCharCode(b);
  return btoa(bin);
}

function b64dec(s: string): Uint8Array {
  const bin = atob(s.trim());
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

/** 32-byte session key from access token + hwid (changes every login/refresh). */
export async function deriveSessionKey(
  secret: string,
  accessToken: string,
  hwid: string,
): Promise<string> {
  const key = await crypto.subtle.importKey(
    'raw',
    enc(secret),
    { name: 'HMAC', hash: 'SHA-256' },
    false,
    ['sign'],
  );
  const sig = new Uint8Array(
    await crypto.subtle.sign('HMAC', key, enc(`${accessToken}|${hwid}`)),
  );
  return b64(sig.slice(0, 32));
}

/** AES-GCM wrap of 32-byte DEK for one session. Returns base64(nonce|cipher+tag). */
export async function wrapDek(sessionKeyB64: string, dekB64: string): Promise<string> {
  const sessionKey = b64dec(sessionKeyB64);
  if (sessionKey.length !== 32) throw new Error('Invalid session key length.');
  const dek = b64dec(dekB64);
  if (dek.length !== 32) throw new Error('Invalid DEK length.');

  const iv = crypto.getRandomValues(new Uint8Array(12));
  const aesKey = await crypto.subtle.importKey('raw', sessionKey, 'AES-GCM', false, ['encrypt']);
  const ct = new Uint8Array(await crypto.subtle.encrypt({ name: 'AES-GCM', iv }, aesKey, dek));

  const packed = new Uint8Array(12 + ct.length);
  packed.set(iv, 0);
  packed.set(ct, 12);
  return b64(packed);
}

export { b64dec as decodeBase64, b64 as encodeBase64 };
