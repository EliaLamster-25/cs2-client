const API = window.location.hostname === 'localhost'
  ? 'http://localhost:8787/v1'
  : `${window.location.origin}/v1`;

async function sha256Hex(text) {
  const buf = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(text));
  return [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, '0')).join('');
}

function saveSession(data) {
  localStorage.setItem('crymore_token', data.access_token);
  localStorage.setItem('crymore_refresh', data.refresh_token);
  localStorage.setItem('crymore_user', data.username || '');
}

function clearSession() {
  localStorage.removeItem('crymore_token');
  localStorage.removeItem('crymore_refresh');
  localStorage.removeItem('crymore_user');
}

function token() {
  return localStorage.getItem('crymore_token');
}

async function api(path, opts = {}) {
  const headers = { 'content-type': 'application/json', ...(opts.headers || {}) };
  if (token() && !headers.authorization) {
    headers.authorization = `Bearer ${token()}`;
  }
  const res = await fetch(`${API}${path}`, { ...opts, headers });
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(data.error || `Request failed (${res.status})`);
  return data;
}

function showMsg(el, text, kind = 'error') {
  if (!el) return;
  el.textContent = text;
  el.className = `msg ${kind}`;
  el.classList.remove('hidden');
}

function requireAuth(redirect = '/login.html') {
  if (!token()) {
    window.location.href = redirect;
    return false;
  }
  return true;
}

async function loginForm(form, msgEl) {
  form.addEventListener('submit', async (e) => {
    e.preventDefault();
    const fd = new FormData(form);
    const username = String(fd.get('username') || '').trim();
    const password = String(fd.get('password') || '');
    try {
      const password_hash = await sha256Hex(password);
      const data = await api('/auth/login', {
        method: 'POST',
        body: JSON.stringify({ username, password_hash }),
      });
      saveSession(data);
      window.location.href = '/dashboard.html';
    } catch (err) {
      showMsg(msgEl, err.message);
    }
  });
}

async function registerForm(form, msgEl) {
  form.addEventListener('submit', async (e) => {
    e.preventDefault();
    const fd = new FormData(form);
    const invite_code = String(fd.get('invite_code') || '').trim();
    const email = String(fd.get('email') || '').trim();
    const username = String(fd.get('username') || '').trim().toLowerCase();
    const password = String(fd.get('password') || '');
    if (password.length < 8) {
      showMsg(msgEl, 'Password must be at least 8 characters.');
      return;
    }
    try {
      const password_hash = await sha256Hex(password);
      const data = await api('/auth/register', {
        method: 'POST',
        body: JSON.stringify({ invite_code, email, username, password_hash }),
      });
      saveSession(data);
      window.location.href = '/dashboard.html';
    } catch (err) {
      showMsg(msgEl, err.message);
    }
  });
}

async function loadDashboard() {
  if (!requireAuth()) return;
  const msg = document.getElementById('msg');
  const userEl = document.getElementById('dash-username-display') || document.getElementById('dash-username');
  const userSidebar = document.getElementById('sidebar-username');
  const emailEl = document.getElementById('dash-email');
  const planEl = document.getElementById('plan');
  const expEl = document.getElementById('expires');
  const daysEl = document.getElementById('days-left');
  const statusEl = document.getElementById('status-badge');
  const hwidEl = document.getElementById('hwid-list');
  const avatarImg = document.getElementById('avatar-img');
  const dlVersionEl = document.getElementById('download-version');
  const dlPublishedEl = document.getElementById('download-published');

  document.querySelectorAll('.sidebar-link[data-panel]').forEach((btn) => {
    btn.addEventListener('click', () => {
      const panelId = btn.getAttribute('data-panel');
      document.querySelectorAll('.sidebar-link[data-panel]').forEach((b) => b.classList.remove('active'));
      btn.classList.add('active');
      document.querySelectorAll('.panel').forEach((p) => p.classList.remove('active'));
      document.getElementById(`panel-${panelId}`)?.classList.add('active');
    });
  });

  try {
    const data = await api('/subscription/status');
    if (userEl) userEl.value = data.username;
    if (userSidebar) userSidebar.textContent = data.username;
    if (emailEl) emailEl.value = data.email || '';
    planEl.textContent = data.plan;
    expEl.textContent = new Date(data.expires_at).toLocaleString();
    if (daysEl) daysEl.textContent = `${data.subscription_days_remaining ?? '—'} days`;
    statusEl.textContent = data.in_grace ? 'grace period' : data.active ? 'active' : 'expired';
    statusEl.className = `badge ${data.active ? 'active' : ''}`;

    if (avatarImg) {
      avatarImg.src = data.avatar_url || '/assets/picture_placeholder.png';
    }

    if (data.in_grace) {
      showMsg(msg, 'Subscription in grace period — renew now to stay covered.', 'warn');
    }

    hwidEl.innerHTML = '';
    (data.hwids || []).forEach((h) => {
      const li = document.createElement('li');
      li.textContent = `${h.hwid.slice(0, 16)}… last seen ${new Date(h.last_seen * 1000).toLocaleDateString()}`;
      hwidEl.appendChild(li);
    });
  } catch (err) {
    showMsg(msg, err.message);
    if (err.message.toLowerCase().includes('unauthorized')) {
      clearSession();
      setTimeout(() => { window.location.href = '/login.html'; }, 800);
    }
  }

  try {
    const release = await api('/downloads/latest');
    if (dlVersionEl) dlVersionEl.textContent = `v${release.version} (${release.channel})`;
    if (dlPublishedEl) dlPublishedEl.textContent = new Date(release.published_at).toLocaleString();
  } catch (err) {
    if (dlVersionEl) dlVersionEl.textContent = 'unavailable';
    if (dlPublishedEl) dlPublishedEl.textContent = err.message || '—';
  }

  document.getElementById('password-form')?.addEventListener('submit', async (e) => {
    e.preventDefault();
    const form = e.target;
    const current = String(new FormData(form).get('current_password') || '');
    const next = String(new FormData(form).get('new_password') || '');
    if (next.length < 8) {
      showMsg(msg, 'New password must be at least 8 characters.');
      return;
    }
    try {
      const current_password_hash = await sha256Hex(current);
      const new_password_hash = await sha256Hex(next);
      const data = await api('/profile/password', {
        method: 'POST',
        body: JSON.stringify({ current_password_hash, new_password_hash }),
      });
      form.reset();
      showMsg(msg, data.message || 'Password updated.', 'ok');
    } catch (err) {
      showMsg(msg, err.message);
    }
  });

  document.getElementById('avatar-file')?.addEventListener('change', async (e) => {
    const file = e.target.files?.[0];
    const nameEl = document.getElementById('avatar-file-name');
    if (!file) {
      if (nameEl) nameEl.textContent = 'no file chosen';
      return;
    }
    if (nameEl) nameEl.textContent = file.name;
    const fd = new FormData();
    fd.append('avatar', file);
    try {
      const res = await fetch(`${API}/profile/avatar`, {
        method: 'POST',
        headers: { authorization: `Bearer ${token()}` },
        body: fd,
      });
      const data = await res.json();
      if (!res.ok) throw new Error(data.error || 'Upload failed');
      if (avatarImg && data.avatar_url) avatarImg.src = `${data.avatar_url}?t=${Date.now()}`;
      showMsg(msg, 'Profile picture updated.', 'ok');
    } catch (err) {
      showMsg(msg, err.message);
    }
  });

  document.getElementById('avatar-remove-btn')?.addEventListener('click', async () => {
    try {
      await api('/profile/avatar', { method: 'DELETE', body: '{}' });
      if (avatarImg) avatarImg.src = '/assets/picture_placeholder.png';
      if (document.getElementById('avatar-file-name')) {
        document.getElementById('avatar-file-name').textContent = 'no file chosen';
      }
      const fileInput = document.getElementById('avatar-file');
      if (fileInput) fileInput.value = '';
      showMsg(msg, 'Profile picture removed.', 'ok');
    } catch (err) {
      showMsg(msg, err.message);
    }
  });

  document.getElementById('renew-btn')?.addEventListener('click', async () => {
    try {
      const data = await api('/subscription/renew', { method: 'POST', body: '{}' });
      showMsg(msg, data.message || 'Renewed.', 'ok');
      loadDashboard();
    } catch (err) {
      showMsg(msg, err.message);
    }
  });

  document.getElementById('download-btn')?.addEventListener('click', async () => {
    try {
      const data = await api('/downloads/loader', { method: 'POST', body: '{}' });
      showMsg(msg, `Downloading ${data.file_name} (v${data.version})…`, 'ok');
      window.location.href = data.download_url;
    } catch (err) {
      showMsg(msg, err.message);
    }
  });

  document.getElementById('hwid-reset-btn')?.addEventListener('click', async () => {
    if (!confirm('Clear all bound PCs? You can log in again on one device.')) return;
    try {
      const data = await api('/subscription/hwid/reset', { method: 'POST', body: '{}' });
      showMsg(msg, data.message, 'ok');
      loadDashboard();
    } catch (err) {
      showMsg(msg, err.message);
    }
  });

  async function refreshCloudConfigs() {
    const list = document.getElementById('cloud-config-list');
    if (!list) return;
    list.innerHTML = '';
    try {
      const data = await api('/configs');
      (data.configs || []).forEach((c) => {
        const li = document.createElement('li');
        li.innerHTML = `<strong>${c.name}</strong> — ${c.description || 'No description'}<br><small>${c.updated_at || ''}</small>`;
        list.appendChild(li);
      });
      if (!(data.configs || []).length) {
        list.innerHTML = '<li>No cloud configs yet.</li>';
      }
    } catch (err) {
      list.innerHTML = `<li>${err.message}</li>`;
    }
  }

  async function refreshLineupPacks() {
    const list = document.getElementById('lineup-pack-list');
    if (!list) return;
    list.innerHTML = '';
    try {
      const data = await api('/lineups');
      (data.packs || []).forEach((p) => {
        const li = document.createElement('li');
        li.innerHTML = `<strong>${p.title}</strong> (${p.map}) — ${p.spot_count} spots · ${p.download_count} downloads`;
        list.appendChild(li);
      });
      if (!(data.packs || []).length) {
        list.innerHTML = '<li>No public lineup packs yet.</li>';
      }
    } catch (err) {
      list.innerHTML = `<li>${err.message}</li>`;
    }
  }

  refreshCloudConfigs();
  refreshLineupPacks();

  document.getElementById('cloud-config-upload-btn')?.addEventListener('click', async () => {
    const name = String(document.getElementById('cloud-config-name')?.value || '').trim();
    const json = String(document.getElementById('cloud-config-json')?.value || '').trim();
    if (!name || !json) {
      showMsg(msg, 'Name and JSON are required.');
      return;
    }
    try {
      JSON.parse(json);
      await api('/configs', {
        method: 'POST',
        body: JSON.stringify({ name, description: '', json, is_public: false }),
      });
      showMsg(msg, 'Config uploaded.', 'ok');
      refreshCloudConfigs();
    } catch (err) {
      showMsg(msg, err.message);
    }
  });

  document.getElementById('lineup-pack-upload-btn')?.addEventListener('click', async () => {
    const title = String(document.getElementById('lineup-pack-title')?.value || '').trim();
    const map = String(document.getElementById('lineup-pack-map')?.value || '').trim();
    const json = String(document.getElementById('lineup-pack-json')?.value || '').trim();
    if (!title || !map || !json) {
      showMsg(msg, 'Title, map, and JSON are required.');
      return;
    }
    try {
      JSON.parse(json);
      await api('/lineups', {
        method: 'POST',
        body: JSON.stringify({ title, map, description: '', grenade_type: 'any', json, is_public: true }),
      });
      showMsg(msg, 'Lineup pack published.', 'ok');
      refreshLineupPacks();
    } catch (err) {
      showMsg(msg, err.message);
    }
  });

  document.getElementById('logout-btn')?.addEventListener('click', () => {
    clearSession();
    window.location.href = '/';
  });
}

window.Crymore = {
  API,
  sha256Hex,
  saveSession,
  clearSession,
  token,
  api,
  loginForm,
  registerForm,
  loadDashboard,
  requireAuth,
};
