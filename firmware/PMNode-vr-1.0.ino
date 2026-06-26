#include <PMNode_inferencing.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <math.h>

// ── ADXL345 ───────────────────────────────────────────────────────
#define ADXL345_ADDR 0x53

// ── Global buffer — avoids stack overflow ─────────────────────────
static float buffer[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE];
static bool debug_nn = false;

// ── NVS for storing credentials ───────────────────────────────────
Preferences prefs;

// ── WebServer (used for both provisioning portal AND dashboard) ───
WebServer server(80);

// ── State machine ─────────────────────────────────────────────────
enum AppState { STATE_PROVISIONING, STATE_RUNNING };
AppState appState = STATE_PROVISIONING;

// ── Label mapping ─────────────────────────────────────────────────
const char* humanLabel(const char* raw) {
    if (strcmp(raw, "SOB_A") == 0) return "Healthy";
    if (strcmp(raw, "SOB_B") == 0) return "Early Degradation";
    if (strcmp(raw, "SOB_C") == 0) return "Severe Degradation";
    if (strcmp(raw, "SOB_D") == 0) return "Failure Imminent";
    return raw;
}

// ── Shared result struct ──────────────────────────────────────────
struct {
    char  top_label[20];
    float scores[EI_CLASSIFIER_LABEL_COUNT];
    float rms_x, rms_y, rms_z;
    int   dsp_ms, classify_ms;
    unsigned long count;
    unsigned long uptime_s;
    bool  ready;
} live;

// ── ADXL345 ───────────────────────────────────────────────────────
void adxl345_init() {
    Wire.beginTransmission(ADXL345_ADDR);
    Wire.write(0x2D); Wire.write(0x08);
    Wire.endTransmission();
    Wire.beginTransmission(ADXL345_ADDR);
    Wire.write(0x2C); Wire.write(0x0C);
    Wire.endTransmission();
}

void readADXL345(float &x, float &y, float &z) {
    Wire.beginTransmission(ADXL345_ADDR);
    Wire.write(0x32);
    Wire.endTransmission(false);
    Wire.requestFrom(ADXL345_ADDR, 6);
    if (Wire.available() >= 6) {
        int16_t rx = Wire.read() | (Wire.read() << 8);
        int16_t ry = Wire.read() | (Wire.read() << 8);
        int16_t rz = Wire.read() | (Wire.read() << 8);
        x = (float)rx;
        y = (float)ry;
        z = (float)rz;
    }
}

// ═════════════════════════════════════════════════════════════════
//  PROVISIONING PORTAL
// ═════════════════════════════════════════════════════════════════

// Returns JSON list of scanned SSIDs
void handleScan() {
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; i++) {
        if (i > 0) json += ",";
        // Escape quotes in SSID just in case
        String ssid = WiFi.SSID(i);
        ssid.replace("\"", "\\\"");
        json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + WiFi.RSSI(i) + "}";
    }
    json += "]";
    WiFi.scanDelete();
    server.send(200, "application/json", json);
}

// Called when user submits credentials
void handleProvisionSave() {
    if (!server.hasArg("ssid") || !server.hasArg("pass")) {
        server.send(400, "text/plain", "Missing ssid or pass");
        return;
    }
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");

    // Save to NVS
    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();

    server.send(200, "text/html",
        "<html><body style='background:#0a0c12;color:#34d399;"
        "font-family:system-ui;text-align:center;padding:40px'>"
        "<h2>&#10003; Saved!</h2>"
        "<p style='color:#e2e2e2'>Connecting to <b>" + ssid + "</b>…<br>"
        "ESP32 will reboot. Reconnect to your normal WiFi<br>"
        "then open the IP printed on Serial Monitor.</p>"
        "</body></html>"
    );

    delay(1500);
    ESP.restart();
}

// The captive-portal setup page
void handlePortal() {
    server.send(200, "text/html", R"===(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PM Node — WiFi Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,sans-serif;background:#0a0c12;color:#e2e2e2;
     display:flex;align-items:center;justify-content:center;min-height:100vh;padding:16px}
.card{background:#13161f;border:0.5px solid #252836;border-radius:12px;
      padding:28px 24px;width:100%;max-width:380px}
h2{font-size:16px;font-weight:500;margin-bottom:4px}
.sub{font-size:12px;color:#4b5268;margin-bottom:20px}
label{font-size:11px;color:#4b5268;text-transform:uppercase;letter-spacing:.06em;display:block;margin-bottom:5px}
select,input{width:100%;background:#0a0c12;border:0.5px solid #252836;border-radius:7px;
             padding:9px 11px;color:#e2e2e2;font-size:13px;margin-bottom:14px;outline:none}
select:focus,input:focus{border-color:#34d399}
.row{display:flex;gap:8px;margin-bottom:14px}
.scan-btn{background:#1a2e24;border:0.5px solid #163d28;border-radius:7px;
          padding:9px 14px;color:#34d399;font-size:12px;cursor:pointer;white-space:nowrap}
.scan-btn:hover{background:#1f3a2c}
button[type=submit]{width:100%;background:#163d28;border:0.5px solid #1f5238;
                    border-radius:7px;padding:11px;color:#34d399;font-size:13px;
                    font-weight:500;cursor:pointer;margin-top:4px}
button[type=submit]:hover{background:#1c4d32}
.net-list{list-style:none;margin-bottom:14px;max-height:180px;overflow-y:auto}
.net-list li{padding:8px 10px;border-radius:6px;cursor:pointer;
             font-size:12px;display:flex;justify-content:space-between;
             border:0.5px solid transparent;margin-bottom:3px;transition:all .15s}
.net-list li:hover{background:#1a2030;border-color:#252836}
.net-list li.sel{background:#0a2318;border-color:#163d28;color:#34d399}
.rssi{font-size:10px;color:#4b5268}
.status{font-size:11px;color:#4b5268;margin-bottom:10px;min-height:16px}
</style>
</head>
<body>
<div class="card">
  <h2>&#9680; PM Node — WiFi Setup</h2>
  <p class="sub">Connect ESP32 to your network</p>

  <label>Available Networks</label>
  <div class="row">
    <select id="net-sel" style="margin-bottom:0" onchange="pickNet(this.value)">
      <option value="">— tap Scan —</option>
    </select>
    <button class="scan-btn" onclick="doScan()">&#8635; Scan</button>
  </div>
  <ul class="net-list" id="net-list"></ul>

  <div class="status" id="status"></div>

  <label>Network Name (SSID)</label>
  <input type="text" id="ssid" placeholder="e.g. HomeNetwork" autocomplete="off">

  <label>Password</label>
  <input type="password" id="pass" placeholder="WiFi password" autocomplete="off">

  <button type="submit" onclick="doSave()">Connect &amp; Save</button>
</div>

<script>
let networks = [];

function doScan() {
  document.getElementById('status').textContent = 'Scanning…';
  document.getElementById('net-list').innerHTML = '';
  document.getElementById('net-sel').innerHTML = '<option>Scanning…</option>';
  fetch('/scan')
    .then(r => r.json())
    .then(nets => {
      networks = nets.sort((a,b) => b.rssi - a.rssi);
      const sel = document.getElementById('net-sel');
      const ul  = document.getElementById('net-list');
      sel.innerHTML = '<option value="">— pick a network —</option>';
      ul.innerHTML  = '';
      nets.forEach(n => {
        const opt = document.createElement('option');
        opt.value = n.ssid; opt.textContent = n.ssid;
        sel.appendChild(opt);
        const li = document.createElement('li');
        const bars = n.rssi > -60 ? '▂▄▆█' : n.rssi > -75 ? '▂▄▆·' : '▂▄··';
        li.innerHTML = '<span>'+n.ssid+'</span><span class="rssi">'+bars+' '+n.rssi+' dBm</span>';
        li.onclick = () => pickNet(n.ssid, li);
        ul.appendChild(li);
      });
      document.getElementById('status').textContent = nets.length + ' network(s) found';
    })
    .catch(() => document.getElementById('status').textContent = 'Scan failed — retry');
}

function pickNet(ssid, liEl) {
  document.getElementById('ssid').value = ssid;
  document.querySelectorAll('.net-list li').forEach(l => l.classList.remove('sel'));
  if (liEl) liEl.classList.add('sel');
  document.getElementById('net-sel').value = ssid;
  document.getElementById('pass').focus();
}

function doSave() {
  const ssid = document.getElementById('ssid').value.trim();
  const pass = document.getElementById('pass').value;
  if (!ssid) { document.getElementById('status').textContent = 'Enter an SSID first'; return; }
  document.getElementById('status').textContent = 'Saving…';
  const fd = new FormData();
  fd.append('ssid', ssid);
  fd.append('pass', pass);
  fetch('/save', { method:'POST', body: new URLSearchParams({ssid, pass}) })
    .then(r => r.text())
    .then(html => { document.open(); document.write(html); document.close(); })
    .catch(() => document.getElementById('status').textContent = 'Error — check connection');
}

// Auto-scan on load
doScan();
</script>
</body>
</html>
)===");
}

// Redirect any unknown URL to portal (captive portal behaviour)
void handleCaptive() {
    server.sendHeader("Location", "http://192.168.4.1/");
    server.send(302, "text/plain", "");
}

// ═════════════════════════════════════════════════════════════════
//  DASHBOARD  (same as before)
// ═════════════════════════════════════════════════════════════════

void handleData() {
    if (!live.ready) {
        server.send(503, "application/json", "{\"status\":\"warming_up\"}");
        return;
    }
    char json[700];
    snprintf(json, sizeof(json),
        "{"
          "\"top\":\"%s\","
          "\"scores\":[%.5f,%.5f,%.5f,%.5f],"
          "\"labels\":[\"Healthy\",\"Early Degradation\",\"Severe Degradation\",\"Failure Imminent\"],"
          "\"rms\":[%.1f,%.1f,%.1f],"
          "\"dsp\":%d,\"cls\":%d,"
          "\"count\":%lu,\"uptime\":%lu,"
          "\"ip\":\"%s\""
        "}",
        live.top_label,
        live.scores[0], live.scores[1], live.scores[2], live.scores[3],
        live.rms_x, live.rms_y, live.rms_z,
        live.dsp_ms, live.classify_ms,
        live.count, live.uptime_s,
        WiFi.localIP().toString().c_str()
    );
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
}

void handleDashboard() {
    // ── Your existing dashboard HTML — paste it here exactly as before ──
    server.send(200, "text/html", R"===(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PM Node</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,sans-serif;background:#0a0c12;padding:10px;color:#e2e2e2;font-size:13px}
.topbar{background:#13161f;border:0.5px solid #252836;border-radius:10px;padding:9px 14px;display:flex;align-items:center;justify-content:space-between;margin-bottom:9px}
.t-title{font-size:13px;font-weight:500;color:#f0f0f0}
.t-sub{font-size:11px;color:#4b5268;margin-top:1px}
.t-right{display:flex;align-items:center;gap:8px}
.conn-badge{font-size:11px;padding:3px 10px;border-radius:20px;display:flex;align-items:center;gap:5px;background:#0a2318;color:#34d399;border:0.5px solid #163d28;transition:all .3s}
.conn-dot{width:6px;height:6px;border-radius:50%;background:#34d399;flex-shrink:0}
.uptime-txt{font-size:11px;color:#4b5268}
.main{display:grid;grid-template-columns:165px 1fr;gap:9px}
.col-l{display:flex;flex-direction:column;gap:9px}
.col-r{display:flex;flex-direction:column;gap:9px}
.card{background:#13161f;border:0.5px solid #252836;border-radius:8px;padding:9px 12px}
.lbl{font-size:10px;color:#4b5268;margin-bottom:3px;text-transform:uppercase;letter-spacing:.05em}
.val{font-size:19px;font-weight:500;color:#f0f0f0;line-height:1.2}
.val span{font-size:12px;font-weight:400;color:#4b5268}
.sub{font-size:10px;color:#4b5268;margin-top:2px}
.health{border-radius:8px;padding:13px 10px;text-align:center;transition:background .5s,border .5s}
.health .hl{font-size:10px;text-transform:uppercase;letter-spacing:.06em;margin-bottom:4px}
.health .hv{font-size:15px;font-weight:500;line-height:1.3}
.health .hc{font-size:11px;margin-top:4px}
.h-ok{background:#0a2318;border:0.5px solid #163d28}
.h-ok .hl,.h-ok .hc{color:#34d399}.h-ok .hv{color:#6ee7b7}
.h-warn{background:#1f1608;border:0.5px solid #3d2c08}
.h-warn .hl,.h-warn .hc{color:#fbbf24}.h-warn .hv{color:#fde68a}
.h-sev{background:#1c0c0c;border:0.5px solid #3d1212}
.h-sev .hl,.h-sev .hc{color:#f87171}.h-sev .hv{color:#fca5a5}
.h-crit{background:#1c080a;border:0.5px solid #5c1010}
.h-crit .hl,.h-crit .hc{color:#ef4444}
.h-crit .hv{color:#fca5a5;animation:pulse 1s ease-in-out infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.4}}
.axis-row{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
.acard{background:#13161f;border:0.5px solid #252836;border-radius:8px;padding:8px 10px}
.acard .al{font-size:10px;color:#4b5268;margin-bottom:5px;text-transform:uppercase;letter-spacing:.04em}
.track{background:#0a0c12;border-radius:3px;height:5px;overflow:hidden;margin-bottom:4px}
.fill{height:5px;border-radius:3px;transition:width .5s ease}
.acard .av{font-size:12px;font-weight:500;color:#e2e2e2}
.pcard{background:#13161f;border:0.5px solid #252836;border-radius:8px;padding:12px 14px}
.pl{font-size:10px;color:#4b5268;margin-bottom:12px;text-transform:uppercase;letter-spacing:.05em}
.chart-wrap{display:flex;align-items:flex-end;gap:0;height:120px;border-bottom:0.5px solid #252836;padding-bottom:0;position:relative}
.bar-col{flex:1;display:flex;flex-direction:column;align-items:center;justify-content:flex-end;height:100%;padding:0 5px;position:relative}
.bar-pct{font-size:11px;font-weight:500;color:#e2e2e2;margin-bottom:5px;transition:all .5s}
.bar-body{width:100%;border-radius:4px 4px 0 0;transition:height .5s ease;min-height:3px;position:relative}
.bar-glow{position:absolute;bottom:0;left:0;right:0;height:30%;border-radius:4px 4px 0 0;opacity:.25}
.bar-labels{display:flex;gap:0;margin-top:8px}
.bar-label-col{flex:1;display:flex;flex-direction:column;align-items:center;padding:0 5px;gap:3px}
.bar-name{font-size:10px;color:#4b5268;text-align:center;line-height:1.3}
.bar-dot{width:6px;height:6px;border-radius:50%;margin-bottom:1px}
.chart-wrap::before{content:'100%';position:absolute;top:-4px;left:0;font-size:9px;color:#2e3347}
.chart-wrap::after{content:'';position:absolute;top:0;left:0;right:0;height:0.5px;background:#1e2130}
.logcard{background:#0d0f17;border:0.5px solid #1e2130;border-radius:8px;padding:9px 12px}
.ll{font-size:10px;color:#4b5268;margin-bottom:6px;text-transform:uppercase;letter-spacing:.05em}
.logbody{font-family:'Courier New',monospace;font-size:11px;color:#4b5268;line-height:1.9}
.logbody .hi{color:#34d399;font-weight:500}
.logbody .dim{color:#2e3347}

/* Reset WiFi button */
.reset-btn{position:fixed;bottom:14px;right:14px;background:#1c0808;border:0.5px solid #3d1010;
           border-radius:7px;padding:7px 13px;color:#f87171;font-size:11px;cursor:pointer;z-index:99}
.reset-btn:hover{background:#2a0c0c}
@media(max-width:500px){.main{grid-template-columns:1fr}}
</style>
</head>
<body>

<div class="topbar">
  <div>
    <div class="t-title">&#9680;&nbsp; PM Node &mdash; Predictive Maintenance</div>
    <div class="t-sub" id="ip-line">Connecting to ESP32&hellip;</div>
  </div>
  <div class="t-right">
    <div class="conn-badge" id="conn-badge">
      <span class="conn-dot" id="conn-dot"></span>
      <span id="conn-label">Connecting</span>
    </div>
    <div class="uptime-txt" id="uptime-el"></div>
  </div>
</div>

<div class="main">
  <div class="col-l">
    <div class="health h-ok" id="health-card">
      <div class="hl">Motor condition</div>
      <div class="hv" id="top-label">&#8212;</div>
      <div class="hc" id="top-conf">Waiting&hellip;</div>
    </div>
    <div class="card">
      <div class="lbl">DSP time</div>
      <div class="val" id="dsp-val">&#8212;<span>ms</span></div>
      <div class="sub">Classify: <span id="cls-val">&#8212;</span> ms</div>
    </div>
    <div class="card">
      <div class="lbl">Window / ODR</div>
      <div class="val">1000<span>ms</span></div>
      <div class="sub">400 Hz &middot; 2.5 ms stride</div>
    </div>
    <div class="card">
      <div class="lbl">Inference count</div>
      <div class="val" id="inf-count">0</div>
      <div class="sub">Since power-on</div>
    </div>
  </div>

  <div class="col-r">
    <div class="axis-row">
      <div class="acard">
        <div class="al">X-axis RMS</div>
        <div class="track"><div class="fill" id="rx-bar" style="width:0%;background:#34d399"></div></div>
        <div class="av" id="rx-val">&#8212;</div>
      </div>
      <div class="acard">
        <div class="al">Y-axis RMS</div>
        <div class="track"><div class="fill" id="ry-bar" style="width:0%;background:#34d399"></div></div>
        <div class="av" id="ry-val">&#8212;</div>
      </div>
      <div class="acard">
        <div class="al">Z-axis RMS</div>
        <div class="track"><div class="fill" id="rz-bar" style="width:0%;background:#60a5fa"></div></div>
        <div class="av" id="rz-val">&#8212;</div>
      </div>
    </div>
    <div class="pcard">
      <div class="pl">Class probabilities</div>
      <div class="chart-wrap">
        <div class="bar-col"><div class="bar-pct" id="bpct0">-</div><div class="bar-body" id="bar0" style="background:#34d399;height:0%"><div class="bar-glow" style="background:#34d399"></div></div></div>
        <div class="bar-col"><div class="bar-pct" id="bpct1">-</div><div class="bar-body" id="bar1" style="background:#fbbf24;height:0%"><div class="bar-glow" style="background:#fbbf24"></div></div></div>
        <div class="bar-col"><div class="bar-pct" id="bpct2">-</div><div class="bar-body" id="bar2" style="background:#f87171;height:0%"><div class="bar-glow" style="background:#f87171"></div></div></div>
        <div class="bar-col"><div class="bar-pct" id="bpct3">-</div><div class="bar-body" id="bar3" style="background:#ef4444;height:0%"><div class="bar-glow" style="background:#ef4444"></div></div></div>
      </div>
      <div class="bar-labels">
        <div class="bar-label-col"><div class="bar-dot" style="background:#34d399"></div><div class="bar-name">Healthy</div></div>
        <div class="bar-label-col"><div class="bar-dot" style="background:#fbbf24"></div><div class="bar-name">Early Degradation</div></div>
        <div class="bar-label-col"><div class="bar-dot" style="background:#f87171"></div><div class="bar-name">Severe Degradation</div></div>
        <div class="bar-label-col"><div class="bar-dot" style="background:#ef4444"></div><div class="bar-name">Failure Imminent</div></div>
      </div>
    </div>
    <div class="logcard">
      <div class="ll">Serial log</div>
      <div class="logbody" id="log-el">Waiting for first inference&hellip;</div>
    </div>
  </div>
</div>

<!-- Reset WiFi button -->
<button class="reset-btn" onclick="if(confirm('Clear WiFi and restart setup?')) fetch('/reset').then(()=>alert('Restarting…'))">
  &#8635; Reset WiFi
</button>

<script>
const RMS_MAX = 700;
const BAR_MAX_H = 100;
function upfmt(s){return String(Math.floor(s/3600)).padStart(2,'0')+':'+String(Math.floor((s%3600)/60)).padStart(2,'0')+':'+String(s%60).padStart(2,'0')}
function healthClass(lbl){if(lbl==='Healthy')return 'h-ok';if(lbl==='Early Degradation')return 'h-warn';if(lbl==='Severe Degradation')return 'h-sev';return 'h-crit'}
function setConn(ok){document.getElementById('conn-dot').style.background=ok?'#34d399':'#ef4444';document.getElementById('conn-label').textContent=ok?'Connected':'Reconnecting…';document.getElementById('conn-badge').style.background=ok?'#0a2318':'#1c0808';document.getElementById('conn-badge').style.color=ok?'#34d399':'#ef4444';document.getElementById('conn-badge').style.borderColor=ok?'#163d28':'#3d1010'}
function poll(){
  fetch('/data').then(r=>{if(!r.ok)throw 0;return r.json()}).then(d=>{
    if(d.status==='warming_up'){setConn(true);document.getElementById('log-el').textContent='Sampling first window…';return}
    setConn(true);
    document.getElementById('ip-line').textContent=d.ip+' · ESP32 · PMNode v1.0.2';
    document.getElementById('uptime-el').textContent='Up '+upfmt(d.uptime);
    const hc=document.getElementById('health-card');hc.className='health '+healthClass(d.top);
    document.getElementById('top-label').textContent=d.top;
    const best=Math.max(...d.scores);document.getElementById('top-conf').textContent='Confidence: '+(best*100).toFixed(1)+'%';
    document.getElementById('dsp-val').innerHTML=d.dsp+'<span>ms</span>';document.getElementById('cls-val').textContent=d.cls;document.getElementById('inf-count').textContent=d.count;
    ['rx','ry','rz'].forEach((ax,i)=>{const pct=Math.min(100,(d.rms[i]/RMS_MAX)*100).toFixed(1);document.getElementById(ax+'-bar').style.width=pct+'%';document.getElementById(ax+'-val').textContent=d.rms[i].toFixed(0)+' ct'});
    const topIdx=d.scores.indexOf(Math.max(...d.scores));
    d.scores.forEach((s,i)=>{const h=Math.max(2,(s*BAR_MAX_H)).toFixed(1);document.getElementById('bar'+i).style.height=h+'%';document.getElementById('bpct'+i).textContent=(s*100).toFixed(1)+'%';document.getElementById('bar'+i).style.opacity=(i===topIdx)?'1':'0.35';document.getElementById('bpct'+i).style.color=(i===topIdx)?'#f0f0f0':'#4b5268'});
    document.getElementById('log-el').innerHTML='<span class="dim">Predictions (DSP: '+d.dsp+'ms, Classify: '+d.cls+'ms):</span><br>'+d.labels.map((l,i)=>{const hi=i===topIdx;return '<span'+(hi?' class="hi"':'')+'>  '+l+': '+d.scores[i].toFixed(5)+(hi?' <<':'')+'</span>'}).join('<br>');
  }).catch(()=>setConn(false));
}
poll();setInterval(poll,2000);
</script>
</body>
</html>
)===");
}

// Clears saved credentials and reboots into provisioning mode
void handleReset() {
    prefs.begin("wifi", false);
    prefs.clear();
    prefs.end();
    server.send(200, "text/plain", "Cleared. Rebooting…");
    delay(500);
    ESP.restart();
}

// ═════════════════════════════════════════════════════════════════
//  SETUP
// ═════════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    while (!Serial);
    Serial.println("PM Node — ESP32 + ADXL345");

    Wire.begin(21, 22);
    adxl345_init();

    if (EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME != 3) {
        Serial.println("ERR: RAW_SAMPLES_PER_FRAME must be 3");
        while(1);
    }

    // ── Try saved credentials ──────────────────────────────────
    prefs.begin("wifi", true);
    String savedSSID = prefs.getString("ssid", "");
    String savedPass = prefs.getString("pass", "");
    prefs.end();

    if (savedSSID.length() > 0) {
        Serial.printf("Trying saved WiFi: %s\n", savedSSID.c_str());
        WiFi.begin(savedSSID.c_str(), savedPass.c_str());

        uint32_t t = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) {
            delay(300); Serial.print(".");
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("\nConnected! Dashboard: http://%s\n",
                          WiFi.localIP().toString().c_str());
            appState = STATE_RUNNING;
            server.on("/",          handleDashboard);
            server.on("/dashboard", handleDashboard);
            server.on("/data",      handleData);
            server.on("/reset",     handleReset);
            server.begin();
            memset(&live, 0, sizeof(live));
            live.ready = false;
            return;
        }
        Serial.println("\nSaved WiFi failed — starting setup portal");
    }

    // ── No creds or connection failed → provisioning AP ───────
    appState = STATE_PROVISIONING;
    WiFi.mode(WIFI_AP);
    WiFi.softAP("PMNode-Setup");          // open AP, no password
    Serial.printf("Setup AP started. Connect to 'PMNode-Setup' → open http://192.168.4.1\n");

    server.on("/",       handlePortal);
    server.on("/scan",   handleScan);
    server.on("/save",   HTTP_POST, handleProvisionSave);
    server.onNotFound(handleCaptive);
    server.begin();
}

// ═════════════════════════════════════════════════════════════════
//  LOOP
// ═════════════════════════════════════════════════════════════════
void loop() {
    server.handleClient();
    if (appState != STATE_RUNNING) return;   // just serve portal, no inference

    Serial.println("\nStarting inferencing in 2 seconds...");
    delay(2000);
    Serial.println("Sampling...");

    memset(buffer, 0, sizeof(buffer));
    float sum_x = 0, sum_y = 0, sum_z = 0;

    for (size_t ix = 0; ix < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE; ix += 3) {
        if (ix % 60 == 0) server.handleClient();

        uint64_t next_tick = micros() + (uint64_t)(EI_CLASSIFIER_INTERVAL_MS * 1000);
        float x, y, z;
        readADXL345(x, y, z);
        buffer[ix + 0] = x;
        buffer[ix + 1] = y;
        buffer[ix + 2] = z;
        sum_x += x * x;
        sum_y += y * y;
        sum_z += z * z;
        int64_t remaining = (int64_t)next_tick - (int64_t)micros();
        if (remaining > 0) delayMicroseconds((uint32_t)remaining);
    }

    uint32_t n = EI_CLASSIFIER_RAW_SAMPLE_COUNT;
    live.rms_x = sqrtf(sum_x / n);
    live.rms_y = sqrtf(sum_y / n);
    live.rms_z = sqrtf(sum_z / n);

    signal_t signal;
    int err = numpy::signal_from_buffer(buffer, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal);
    if (err != 0) { Serial.printf("Failed to create signal (%d)\n", err); return; }

    ei_impulse_result_t result = { 0 };
    err = run_classifier(&signal, &result, debug_nn);
    if (err != EI_IMPULSE_OK) { Serial.printf("ERR: Classifier failed (%d)\n", err); return; }

    live.dsp_ms      = result.timing.dsp;
    live.classify_ms = result.timing.classification;
    live.count++;
    live.uptime_s    = millis() / 1000;

    float best = -1;
    for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
        live.scores[i] = result.classification[i].value;
        if (live.scores[i] > best) {
            best = live.scores[i];
            strncpy(live.top_label, humanLabel(result.classification[i].label), 19);
        }
    }
    live.ready = true;

    Serial.printf("Predictions (DSP: %d ms, Classification: %d ms):\n",
        result.timing.dsp, result.timing.classification);
    for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
        Serial.printf("  %s → %s: %.5f\n",
            result.classification[i].label,
            humanLabel(result.classification[i].label),
            result.classification[i].value);
    }

#if EI_CLASSIFIER_HAS_ANOMALY == 1
    Serial.printf("  anomaly score: %.3f\n", result.anomaly);
#endif
}