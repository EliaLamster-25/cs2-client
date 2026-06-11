export type Env = {
  DB: D1Database;
  LOADER_BUCKET: R2Bucket;
  ASSETS: Fetcher;
  JWT_SECRET: string;
  ADMIN_SECRET: string;
  PAYLOAD_KEY?: string;
  SITE_URL: string;
  /** Cloudflare account ID (for R2 presigned download URLs). */
  R2_ACCOUNT_ID?: string;
  /** R2 S3 API token — Object Read on loader bucket. */
  R2_ACCESS_KEY_ID?: string;
  R2_SECRET_ACCESS_KEY?: string;
  R2_BUCKET_NAME?: string;
};

export type UserRow = {
  id: string;
  email: string;
  username: string;
  password_hash: string;
  banned: number;
  created_at: number;
  avatar_key: string | null;
};

export type SubscriptionRow = {
  user_id: string;
  plan: string;
  expires_at: number;
  renewed_at: number | null;
  stripe_customer_id: string | null;
  stripe_subscription_id: string | null;
};

export type JwtClaims = {
  sub: string;
  username: string;
  plan: string;
  sub_exp: number;
};
