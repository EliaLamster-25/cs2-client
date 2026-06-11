const $ = (id) => document.getElementById(id);

const els = {
  pageLogin: $("page-login"),
  pageStatus: $("page-status"),
  loginForm: $("login-form"),
  username: $("username"),
  password: $("password"),
  showPassword: $("show-password"),
  loginError: $("login-error"),
  btnLogin: $("btn-login"),
  btnLaunch: $("btn-launch"),
  btnLogout: $("btn-logout"),
  btnClose: $("btn-close"),
  welcome: $("welcome-title"),
  dotSession: $("dot-session"),
  dotEnv: $("dot-env"),
  dotPhase: $("dot-phase"),
  session: $("stat-session"),
  env: $("stat-env"),
  phase: $("stat-phase"),
  envBanner: $("env-banner"),
  envTitle: $("env-title"),
  envText: $("env-text"),
  progressLabel: $("progress-label"),
  progressFill: $("progress-fill"),
  logLines: $("log-lines"),
};

const hasWebView = !!(window.chrome && window.chrome.webview);

function send(msg) {
  if (hasWebView) window.chrome.webview.postMessage(JSON.stringify(msg));
}

function setDot(el, kind) {
  if (el) el.className = "stat-dot" + (kind ? " " + kind : "");
}

function phaseLabel(phase) {
  switch (phase) {
    case "boot": return "booting";
    case "login": return "idle";
    case "ready": return "ready";
    case "launching": return "injecting";
    case "done": return "injected";
    case "failed": return "failed";
    default: return phase || "idle";
  }
}

function progressForPhase(phase, busy) {
  if (phase === "done") return { pct: 100, label: "Complete", cls: "done" };
  if (phase === "failed") return { pct: 100, label: "Failed", cls: "error" };
  if (phase === "launching" || busy) return { pct: 65, label: "Injecting…", cls: "busy" };
  if (phase === "ready") return { pct: 25, label: "Ready", cls: "" };
  return { pct: 10, label: "Waiting", cls: "" };
}

function setPage(loggedIn) {
  els.pageLogin.classList.toggle("hidden", loggedIn);
  els.pageStatus.classList.toggle("hidden", !loggedIn);
}

function renderLogs(lines) {
  const filtered = (lines || []).filter((l) =>
    l.startsWith("[launch]") || l.startsWith("[env]") ||
    l.startsWith("[warn]") || l.startsWith("[auth]") || l.startsWith("[boot]")
  );
  els.logLines.textContent = filtered.slice(-8).join("\n");
}

function render(state) {
  const loggedIn = !!state.loggedIn;
  const busy = !!state.busy;
  const envOk = !!state.envOk;
  const phase = state.phase || "idle";

  setPage(loggedIn);

  if (!loggedIn) {
    els.btnLogin.disabled = busy;
    const label = els.btnLogin.querySelector(".btn-label");
    if (label) label.textContent = busy ? "Signing in…" : "Sign in";
    if (state.status && state.status.indexOf("Sign in") === -1) {
      els.loginError.textContent = state.status;
    } else {
      els.loginError.textContent = "";
    }
    return;
  }

  // page 2
  if (state.username) {
    els.welcome.textContent = state.username;
  }

  els.session.textContent = "authorized";
  els.session.className = "stat-value ok";
  setDot(els.dotSession, "ok");

  els.env.textContent = envOk ? "clear" : "blocked";
  els.env.className = "stat-value" + (envOk ? " ok" : " bad");
  setDot(els.dotEnv, envOk ? "ok" : "bad");

  const pLabel = phaseLabel(phase);
  els.phase.textContent = pLabel;
  els.phase.className = "stat-value" +
    (phase === "failed" ? " bad" : phase === "launching" ? " busy" : phase === "done" ? " ok" : "");
  setDot(els.dotPhase, phase === "failed" ? "bad" : phase === "launching" ? "busy" : phase === "done" ? "ok" : "");

  els.envBanner.className = "inject-banner" +
    (!envOk ? " bad" : phase === "launching" ? " busy" : "");
  els.envTitle.textContent = !envOk
    ? "Environment blocked"
    : phase === "launching"
      ? "Injecting overlay"
      : phase === "done"
        ? "Injection complete"
        : phase === "failed"
          ? "Injection failed"
          : "Ready to inject";
  els.envText.textContent = !envOk
    ? "Close FACEIT / Vanguard / EAC first."
    : phase === "launching"
      ? "Extracting payload and starting process…"
      : phase === "done"
        ? "Overlay is running."
        : phase === "failed"
          ? (state.status || "Something went wrong.")
          : "No protected services detected.";

  const prog = progressForPhase(phase, busy);
  els.progressLabel.textContent = prog.label;
  els.progressFill.style.width = prog.pct + "%";
  els.progressFill.className = "progress-fill" + (prog.cls ? " " + prog.cls : "");

  els.btnLaunch.disabled = busy || !envOk || phase === "done";
  const launchLabel = els.btnLaunch.querySelector(".btn-label");
  if (launchLabel) {
    launchLabel.textContent = phase === "launching" || busy
      ? "Injecting…"
      : phase === "done"
        ? "Running"
        : "Inject overlay";
  }

  renderLogs(state.logs);
}

els.showPassword.addEventListener("change", () => {
  els.password.type = els.showPassword.checked ? "text" : "password";
});

els.loginForm.addEventListener("submit", (e) => {
  e.preventDefault();
  send({ type: "login", username: els.username.value.trim(), password: els.password.value });
});

els.btnLaunch.addEventListener("click", () => send({ type: "launch" }));
els.btnLogout.addEventListener("click", () => send({ type: "logout" }));
els.btnClose.addEventListener("click", () => send({ type: "close" }));

if (hasWebView) {
  window.chrome.webview.addEventListener("message", (ev) => {
    try { render(JSON.parse(ev.data)); } catch (_) {}
  });
  send({ type: "ready" });
} else {
  // browser preview: L = login page, S = status page
  let preview = "login";
  const states = {
    login: { phase: "login", loggedIn: false, envOk: true, busy: false, status: "Sign in to continue.", logs: [] },
    status: {
      phase: "ready", loggedIn: true, envOk: true, busy: false,
      username: "demo", status: "Authenticated.",
      logs: ["[boot] crymore.pw loader", "[auth] welcome, demo", "[env] environment clear"],
    },
    injecting: {
      phase: "launching", loggedIn: true, envOk: true, busy: true,
      username: "demo", status: "Launching overlay…",
      logs: ["[boot] crymore.pw loader", "[launch] extracting payload", "[launch] starting process"],
    },
  };
  const show = (key) => { preview = key; render(states[key]); };
  show("login");
  document.addEventListener("keydown", (e) => {
    if (e.key === "l" || e.key === "L") show("login");
    if (e.key === "s" || e.key === "S") show("status");
    if (e.key === "i" || e.key === "I") show("injecting");
  });
}
