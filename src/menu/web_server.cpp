#include "web_server.h"
#include "config.h"
#include "httplib.h"
#include "json.hpp"
#include <thread>
#include <iostream>

using json = nlohmann::json;

static std::thread g_serverThread;
static httplib::Server g_svr;

// Ultra-modern Web UI
static const char* kHtmlContent = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>CS2 External Config</title>
<link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;600&display=swap" rel="stylesheet">
<style>
  :root {
    --bg-color: #0B0E14;
    --card-bg: rgba(20, 25, 35, 0.7);
    --primary: #6366f1;
    --primary-hover: #818cf8;
    --text-main: #f8fafc;
    --text-muted: #94a3b8;
  }
  * { box-sizing: border-box; font-family: 'Inter', sans-serif; }
  body {
    background: radial-gradient(circle at top left, #1e1b4b, var(--bg-color) 40%);
    color: var(--text-main);
    margin: 0; padding: 2rem;
    min-height: 100vh;
    display: flex; justify-content: center; align-items: flex-start;
  }
  .container {
    width: 100%; max-width: 500px;
    background: var(--card-bg);
    backdrop-filter: blur(16px);
    border: 1px solid rgba(255, 255, 255, 0.05);
    border-radius: 16px;
    padding: 2rem;
    box-shadow: 0 25px 50px -12px rgba(0, 0, 0, 0.5);
  }
  h1 { margin-top: 0; font-size: 1.5rem; border-bottom: 1px solid rgba(255,255,255,0.1); padding-bottom: 1rem; margin-bottom: 1.5rem;}
  .setting { display: flex; justify-content: space-between; align-items: center; margin-bottom: 1rem; }
  
  /* Modern Toggle Switch */
  .switch { position: relative; display: inline-block; width: 50px; height: 26px; }
  .switch input { opacity: 0; width: 0; height: 0; }
  .slider {
    position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0;
    background-color: #334155; transition: .3s; border-radius: 34px;
  }
  .slider:before {
    position: absolute; content: ""; height: 18px; width: 18px;
    left: 4px; bottom: 4px; background-color: white; transition: .3s; border-radius: 50%;
  }
  input:checked + .slider { background-color: var(--primary); }
  input:checked + .slider:before { transform: translateX(24px); }
  
  .val-slider {
    width: 100%; height: 6px; background: #334155; border-radius: 4px;
    outline: none; -webkit-appearance: none;
  }
  .val-slider::-webkit-slider-thumb {
    -webkit-appearance: none; appearance: none;
    width: 16px; height: 16px; border-radius: 50%;
    background: var(--primary); cursor: pointer;
  }
  
  .group { margin-bottom: 2rem; }
  .group-title { font-size: 0.85rem; text-transform: uppercase; color: var(--text-muted); margin-bottom: 1rem; letter-spacing: 1px;}
</style>
</head>
<body>

<div class="container">
  <h1>Control Center</h1>
  
  <div class="group">
    <div class="group-title">Visibility</div>
    <div class="setting">
      <span>Master ESP</span>
      <label class="switch"><input type="checkbox" id="espEnabled" onchange="update()"><span class="slider"></span></label>
    </div>
    <div class="setting">
      <span>Bounding Boxes</span>
      <label class="switch"><input type="checkbox" id="boxEnabled" onchange="update()"><span class="slider"></span></label>
    </div>
    <div class="setting">
      <span>Health Bars</span>
      <label class="switch"><input type="checkbox" id="hpBarEnabled" onchange="update()"><span class="slider"></span></label>
    </div>
    <div class="setting">
      <span>Show Dormant</span>
      <label class="switch"><input type="checkbox" id="showDormant" onchange="update()"><span class="slider"></span></label>
    </div>
    <div class="setting">
      <span>Enemies Only</span>
      <label class="switch"><input type="checkbox" id="enemyOnly" onchange="update()"><span class="slider"></span></label>
    </div>
  </div>

  <div class="group">
    <div class="group-title">Style</div>
    <div style="margin-bottom: 1rem;">
      <div style="display: flex; justify-content: space-between; margin-bottom: 0.5rem;">
        <span>Box Thickness</span>
        <span id="thickVal" style="color: var(--primary);">1.5</span>
      </div>
      <input type="range" id="boxThickness" class="val-slider" min="0.5" max="5.0" step="0.1" oninput="update()">
    </div>
  </div>
</div>

<script>
  // Fetch initial config
  fetch('/api/config').then(r => r.json()).then(cfg => {
    document.getElementById('espEnabled').checked = cfg.espEnabled;
    document.getElementById('boxEnabled').checked = cfg.boxEnabled;
    document.getElementById('hpBarEnabled').checked = cfg.hpBarEnabled;
    document.getElementById('showDormant').checked = cfg.showDormant;
    document.getElementById('enemyOnly').checked = cfg.enemyOnly;
    document.getElementById('boxThickness').value = cfg.boxThickness;
    document.getElementById('thickVal').innerText = cfg.boxThickness;
  });

  function update() {
    const cfg = {
      espEnabled: document.getElementById('espEnabled').checked,
      boxEnabled: document.getElementById('boxEnabled').checked,
      hpBarEnabled: document.getElementById('hpBarEnabled').checked,
      showDormant: document.getElementById('showDormant').checked,
      enemyOnly: document.getElementById('enemyOnly').checked,
      boxThickness: parseFloat(document.getElementById('boxThickness').value)
    };
    document.getElementById('thickVal').innerText = cfg.boxThickness;
    
    fetch('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(cfg)
    });
  }
</script>
</body>
</html>
)HTML";

void WebServer::start() {
    g_svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(kHtmlContent, "text/html");
    });

    g_svr.Get("/api/config", [](const httplib::Request&, httplib::Response& res) {
        json j;
        j["espEnabled"]   = g_cfg.espEnabled;
        j["boxEnabled"]   = g_cfg.boxEnabled;
        j["hpBarEnabled"] = g_cfg.hpBarEnabled;
        j["showDormant"]  = g_cfg.showDormant;
        j["enemyOnly"]    = g_cfg.enemyOnly;
        j["boxThickness"] = g_cfg.boxThickness;
        res.set_content(j.dump(), "application/json");
    });

    g_svr.Post("/api/config", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = json::parse(req.body);
            if (j.contains("espEnabled"))   g_cfg.espEnabled   = j["espEnabled"];
            if (j.contains("boxEnabled"))   g_cfg.boxEnabled   = j["boxEnabled"];
            if (j.contains("hpBarEnabled")) g_cfg.hpBarEnabled = j["hpBarEnabled"];
            if (j.contains("showDormant"))  g_cfg.showDormant  = j["showDormant"];
            if (j.contains("enemyOnly"))    g_cfg.enemyOnly    = j["enemyOnly"];
            if (j.contains("boxThickness")) g_cfg.boxThickness = j["boxThickness"];
            res.status = 200;
        } catch (...) {
            res.status = 400;
        }
    });

    g_serverThread = std::thread([]() {
        std::cout << "[WebServer] Started at http://localhost:8080\n";
        g_svr.listen("0.0.0.0", 8080);
    });
}

void WebServer::stop() {
    g_svr.stop();
    if (g_serverThread.joinable())
        g_serverThread.join();
}
