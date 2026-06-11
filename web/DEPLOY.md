# crymore.pw — deploy walkthrough

Everything in `web/` is the platform (site + API). Follow these steps once; day-to-day dev is `npm run dev` in `web/`.

---

## What you need

- [Cloudflare account](https://dash.cloudflare.com/sign-up) (free)
- Domain **crymore.pw** on Cloudflare DNS (or change names in `wrangler.toml` / loader)
- [Node.js 20+](https://nodejs.org/)
- Built loader: `.\loader\tools\build_ship.ps1`

---

## Step 1 — Install & run locally

```powershell
cd web
npm install
npm run db:local          # creates local D1 schema
npx wrangler secret put JWT_SECRET --local
# enter a long random string (32+ chars)

# optional local admin secret
echo 'ADMIN_SECRET=your-admin-pass' > .dev.vars

npm run dev
```

Open http://localhost:8787

Create first invite (local):

```powershell
curl -X POST http://localhost:8787/v1/admin/bootstrap `
  -H "x-admin-secret: your-admin-pass"
```

Register at http://localhost:8787/register.html with that code.

**Test loader against local API:**

```powershell
$env:CRYMORE_API_HOST = "localhost:8787"
$env:CRYMORE_API_TLS = "0"
# build Release loader, run Crymore.Loader.exe
```

---

## Step 2 — Cloudflare setup (one time)

### 2a. Login

```powershell
cd web
npx wrangler login
```

### 2b. Create D1 database

```powershell
npx wrangler d1 create crymore
```

Copy the `database_id` into `web/wrangler.toml` → `[[d1_databases]]` → `database_id`.

Apply schema:

```powershell
npm run db:remote
```

### 2c. Create R2 bucket

```powershell
npx wrangler r2 bucket create crymore-loaders
```

Name must match `bucket_name` in `wrangler.toml`.

### 2d. Set production secrets

```powershell
npx wrangler secret put JWT_SECRET
npx wrangler secret put ADMIN_SECRET
```

Use strong random values. **Save ADMIN_SECRET** — you need it for invites and uploads.

### 2e. Deploy worker

```powershell
npm run deploy
```

Note the `*.workers.dev` URL for testing before DNS.

---

## Step 3 — DNS (your domain)

In Cloudflare dashboard → DNS:

| Type | Name | Target |
|------|------|--------|
| CNAME or A | `@` | Cloudflare Pages/Workers route for site |
| CNAME | `api` | same worker (or route `api.crymore.pw/*` to worker) |

**Simplest:** use Workers custom domains:

1. Workers & Pages → `crymore-api` → Settings → Domains & Routes
2. Add `crymore.pw` and `api.crymore.pw` (or serve both from one hostname)

If site and API share one worker (this repo), you can use **only `crymore.pw`** and the loader can point to `crymore.pw/v1/...` — update `auth.cpp` host if you skip the `api.` subdomain.

Recommended production:

- `crymore.pw` → worker (static + `/v1/*`)
- Loader default host: `crymore.pw` path `/v1` **or** `api.crymore.pw`

---

## Step 4 — First invite (production)

```powershell
curl -X POST https://crymore.pw/v1/admin/bootstrap `
  -H "x-admin-secret: YOUR_ADMIN_SECRET"
```

Or create custom invites:

```powershell
curl -X POST https://crymore.pw/v1/admin/invites `
  -H "x-admin-secret: YOUR_ADMIN_SECRET" `
  -H "content-type: application/json" `
  -d "{\"max_uses\":5,\"expires_days\":30,\"note\":\"friends\"}"
```

---

## Step 5 — Publish loader download (one command)

From `web/` (builds overlay + loader, uploads to R2, registers release):

```powershell
cd web
npm run publish:loader
```

**Setup once:**

| File | Purpose |
|------|---------|
| `web/.dev.vars` | `ADMIN_SECRET=...` (same as `wrangler secret put ADMIN_SECRET`) |
| `loader/.payload_key` | Same value as `wrangler secret put PAYLOAD_KEY` |

The publish script loads both automatically. Version is read from `web/VERSION` (currently shown on dashboard download).

To bump patch before publish: `npm run publish:loader -- -BumpPatch`

### Manual steps (optional)

```powershell
.\loader\tools\build_ship.ps1
```

### 5b. Pack zip

```powershell
.\web\scripts\pack_loader.ps1
```

Output: `web/dist/crymore-loader.zip`

### 5c. Upload to R2

```powershell
cd web
npx wrangler r2 object put crymore-loaders/releases/stable/1.0.0/crymore-loader.zip --file=dist/crymore-loader.zip
```

### 5d. Register release in API

```powershell
curl -X POST https://crymore.pw/v1/admin/releases `
  -H "x-admin-secret: YOUR_ADMIN_SECRET" `
  -H "content-type: application/json" `
  -d "{\"version\":\"1.0.0\",\"channel\":\"stable\",\"r2_key\":\"releases/stable/1.0.0/crymore-loader.zip\",\"file_name\":\"crymore-loader.zip\"}"
```

Users can now download from the dashboard.

---

## Step 6 — Ship loader to users

Release build with **no dev bypass**:

```powershell
.\loader\tools\build_ship.ps1
```

Distribute **only** via website download (or rebuild zip after each update).

Loader talks to production API automatically (`api.crymore.pw` or env override).

---

## Ongoing operations

| Task | Command / action |
|------|------------------|
| New loader version | `build_ship.ps1` → `pack_loader.ps1` → R2 upload → `POST /v1/admin/releases` |
| New invite batch | `POST /v1/admin/invites` |
| Ban user | `UPDATE users SET banned=1 WHERE username='x'` in D1 dashboard (or add admin route later) |
| Stats | `GET /v1/admin/stats` with admin header |
| Rotate JWT secret | `wrangler secret put JWT_SECRET` (invalidates all sessions) |

---

## Costs

Cloudflare free tier covers early traffic. You pay:

- Domain (~$10–15/year)
- Stripe fees only when you add paid beta/donations later

---

## Troubleshooting

| Problem | Fix |
|---------|-----|
| Loader “server unavailable” | API not deployed; check DNS and `GET /v1/health` |
| Download 503 | No release row or missing R2 object |
| HWID limit | User clicks “reset hwid” on dashboard (30-day cooldown) |
| Login works on site, not loader | Build **Release** loader; Debug allows offline demo |
| Local dev | `CRYMORE_API_HOST` + `CRYMORE_API_TLS=0` |

---

## File map

```
web/
  src/index.ts          Worker entry (API + static)
  src/routes/           auth, subscription, downloads, admin
  schema.sql            D1 tables
  public/               marketing + dashboard HTML
  wrangler.toml         Cloudflare config (edit D1 id)
  scripts/pack_loader.ps1
```

Loader integration: `loader/src/auth.cpp` → `POST /v1/auth/login|validate|refresh`
