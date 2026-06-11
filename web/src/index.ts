import { Hono } from 'hono';
import type { Env } from './types';
import { corsHeaders } from './lib/util';
import { auth } from './routes/auth';
import { sub } from './routes/subscription';
import { downloads } from './routes/downloads';
import { admin } from './routes/admin';
import { profile } from './routes/profile';
import { configs } from './routes/configs';
import { lineups } from './routes/lineups';

const app = new Hono<{ Bindings: Env }>();

app.use('*', async (c, next) => {
  if (c.req.method === 'OPTIONS') {
    return new Response(null, { status: 204, headers: corsHeaders(c.req.header('origin') ?? null) });
  }
  await next();
  c.res.headers.set('access-control-allow-origin', c.req.header('origin') ?? '*');
  c.res.headers.set('access-control-allow-headers', 'content-type, authorization, x-admin-secret');
});

app.route('/v1/auth', auth);
app.route('/v1/subscription', sub);
app.route('/v1/downloads', downloads);
app.route('/v1/admin', admin);
app.route('/v1/profile', profile);
app.route('/v1/configs', configs);
app.route('/v1/lineups', lineups);

app.get('/v1/health', (c) => c.json({ ok: true, service: 'crymore' }));

// Static site — API paths handled above; everything else from /public
app.all('*', async (c) => {
  if (c.req.path.startsWith('/v1/')) {
    return c.json({ ok: false, error: 'Not found.' }, 404);
  }
  return c.env.ASSETS.fetch(c.req.raw);
});

export default app;
