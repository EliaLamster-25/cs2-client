import { AwsClient } from 'aws4fetch';

import type { Env } from '../types';

/** Direct R2 GET URL (bypasses Worker proxy — much faster for large files). */
export async function r2PresignedGetUrl(
  env: Env,
  key: string,
  expiresIn: number,
  downloadName?: string,
): Promise<string | null> {
  const accountId = env.R2_ACCOUNT_ID?.trim();
  const accessKeyId = env.R2_ACCESS_KEY_ID?.trim();
  const secretAccessKey = env.R2_SECRET_ACCESS_KEY?.trim();
  const bucket = env.R2_BUCKET_NAME?.trim() || 'crymore-loaders';
  if (!accountId || !accessKeyId || !secretAccessKey) return null;

  const client = new AwsClient({
    accessKeyId,
    secretAccessKey,
    service: 's3',
    region: 'auto',
  });

  const objectPath = key.split('/').map(encodeURIComponent).join('/');
  let url =
    `https://${accountId}.r2.cloudflarestorage.com/${bucket}/${objectPath}?X-Amz-Expires=${expiresIn}`;
  if (downloadName) {
    const disp = `attachment; filename="${downloadName.replace(/"/g, '')}"`;
    url += `&response-content-disposition=${encodeURIComponent(disp)}`;
  }

  const signed = await client.sign(new Request(url, { method: 'GET' }), {
    aws: { signQuery: true },
  });
  return signed.url;
}
