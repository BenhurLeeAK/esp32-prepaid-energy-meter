#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ======================= SETTINGS =======================

// ---------- Hotspot (the ESP32 creates this WiFi network) ----------
const char* AP_SSID = "Prepaid-Meter";
const char* AP_PASS = "12345678";        // at least 8 characters, or "" for an open hotspot

// ---------- Prepaid rules ----------
const uint32_t RATE_RS_PER_HOUR = 100;   // Rs 100 = 1 hour of supply
const uint32_t LOW_WARN_SEC     = 300;   // low-balance warning when 5 minutes are left
const uint32_t MAX_SECONDS      = 7UL * 24 * 3600;   // balance cap: 7 days
const uint32_t MAX_RECHARGE_RS  = 5000;  // largest single recharge
const char*    ADMIN_PIN        = "1234";  // needed to recharge or clear; "" turns the PIN off
const uint32_t SAVE_EVERY_S     = 30;    // save the balance to flash every 30 s while running

// ---------- Pins ----------
const int CURRENT_PIN = 32;   // ACS712 OUT
const int VOLT_PIN    = 35;   // 0-25V voltage sensor module S pin
const int RELAY_PIN   = 23;
const int SWITCH_PIN  = 14;   // push button between this pin and GND
const int BUZZER_PIN  = 18;

// Most 5V relay modules are active LOW. Swap if yours is opposite.
const int RELAY_ON  = LOW;
const int RELAY_OFF = HIGH;

// Buzzer modules with a transistor are usually active LOW (they sound when the pin is LOW).
// If the buzzer is silent when it should beep, or sounds all the time, swap these two.
const int BUZZER_ON  = LOW;
const int BUZZER_OFF = HIGH;

// ---------- LCD (from your code) ----------
LiquidCrystal_I2C lcd(0x27, 16, 2);   // use 0x3F if the LCD stays blank
const int LCD_SDA = 21;
const int LCD_SCL = 22;

// ---------- Voltage sensor (0-25V DC module, divides by 5) ----------
const float VOLT_DIVIDER_RATIO = 5.0;    // Vin = Vpin x 5
const float SIM_INPUT_FOR_230V = 12.0;   // module input voltage that is shown as 230 V (keep input below about 16 V)

// ---------- Current sensor (ACS712) ----------
const float ACS_DIVIDER  = 1.0;     // set 0.667 if you added a 10k/20k divider on ACS712 OUT
const float ACS_MV_PER_A = 185.0;   // 5A = 185, 20A = 100, 30A = 66

// ---------- Switch timing ----------
const unsigned long DEBOUNCE_MS   = 40;
const unsigned long LONG_PRESS_MS = 3000;   // hold 3 s = restart (balance is kept)

// ======================= STATE =======================

WebServer server(80);
DNSServer dnsServer;
Preferences prefs;

uint32_t remaining = 0;      // seconds of supply left
uint32_t capSec = 0;         // size of the last top-up (for the progress ring)
bool running = false;        // supply is switched on by the user
bool relayState = false;
uint32_t totalRs = 0;
uint32_t recharges = 0;
float energyWh = 0;

float vPinMv = 0, vin = 0, volts = 0;
float acsMean = 0, acsZero = 0, amps = 0, watts = 0;

unsigned long lastMeasure = 0;
unsigned long lastTick = 0;
unsigned long lastPrint = 0;

// LCD
const int NUM_PAGES = 4;
int currentPage = 0;
char flashL0[17] = "", flashL1[17] = "";
unsigned long flashUntil = 0;

// Switch
bool btnLastRead = HIGH;
bool btnStable = HIGH;
unsigned long btnLastChange = 0;
unsigned long btnPressedAt = 0;
bool longHandled = false;

// Buzzer
int beepsLeft = 0;
bool beepOn = false;
unsigned long beepTimer = 0;

// ======================= HELPERS =======================

float balanceRs() {
  return (float)remaining * (float)RATE_RS_PER_HOUR / 3600.0f;
}

float readAvgMv(int pin, int samples = 64) {
  uint32_t sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogReadMilliVolts(pin);
  }
  return (float)sum / samples;
}

// Take the zero-current level of the ACS712 (nothing should be flowing through it now)
void zeroCurrent() {
  acsZero = readAvgMv(CURRENT_PIN, 512);
  Serial.printf("Current zero level: %.0f mV\n", acsZero);
}

// Relay is ON only while the user has supply running AND there is time left
void applyRelay() {
  relayState = running && remaining > 0;
  digitalWrite(RELAY_PIN, relayState ? RELAY_ON : RELAY_OFF);
}

// ---------- Buzzer (non-blocking beeps) ----------
void startBeeps(int n) {
  beepsLeft = n;
  beepTimer = millis() - 200;   // first beep starts right away
}

void updateBuzzer() {
  if (beepsLeft <= 0 && !beepOn) return;
  unsigned long now = millis();
  if (!beepOn) {
    if (beepsLeft > 0 && now - beepTimer >= 200) {
      beepOn = true;
      beepTimer = now;
      digitalWrite(BUZZER_PIN, BUZZER_ON);
    }
  } else if (now - beepTimer >= 150) {
    beepOn = false;
    beepTimer = now;
    digitalWrite(BUZZER_PIN, BUZZER_OFF);
    beepsLeft--;
  }
}

// ---------- Saved balance ----------
void saveState() {
  prefs.putUInt("rem", remaining);
  prefs.putBool("run", running);
  prefs.putUInt("cap", capSec);
  prefs.putUInt("tot", totalRs);
  prefs.putUInt("cnt", recharges);
}

void loadState() {
  remaining = prefs.getUInt("rem", 0);
  running = prefs.getBool("run", false);
  capSec = prefs.getUInt("cap", remaining);
  totalRs = prefs.getUInt("tot", 0);
  recharges = prefs.getUInt("cnt", 0);
  if (remaining == 0) running = false;
  if (capSec < remaining) capSec = remaining;
  Serial.printf("Restored balance: %lu s, running=%d\n", (unsigned long)remaining, running ? 1 : 0);
}

// ---------- LCD ----------
void drawLine(uint8_t row, const char* text) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16s", text);   // pad or cut to 16 characters
  lcd.setCursor(0, row);
  lcd.print(buf);
}

void setFlash(const char* a, const char* b, unsigned long ms) {
  strncpy(flashL0, a, 16); flashL0[16] = 0;
  strncpy(flashL1, b, 16); flashL1[16] = 0;
  flashUntil = millis() + ms;
}

void drawPage() {
  if ((long)(flashUntil - millis()) > 0) {
    drawLine(0, flashL0);
    drawLine(1, flashL1);
    return;
  }

  char l0[40], l1[40];
  bool low = remaining > 0 && remaining <= LOW_WARN_SEC;

  switch (currentPage) {
    case 0:   // Balance and time left
      if (remaining == 0) {
        snprintf(l0, sizeof(l0), "RECHARGE NEEDED");
        snprintf(l1, sizeof(l1), "Balance Rs 0.00");
      } else {
        if (low && ((millis() / 1000) % 2 == 0)) snprintf(l0, sizeof(l0), "LOW BALANCE");
        else snprintf(l0, sizeof(l0), "Rs %7.2f %s", balanceRs(), running ? "RUN" : "HOLD");
        snprintf(l1, sizeof(l1), "Left %lu:%02lu:%02lu",
                 (unsigned long)(remaining / 3600),
                 (unsigned long)((remaining % 3600) / 60),
                 (unsigned long)(remaining % 60));
      }
      break;
    case 1:   // Voltage (scaled to 230 V) and current
      snprintf(l0, sizeof(l0), "V:%7.1f V", volts);
      snprintf(l1, sizeof(l1), "I:%7.2f A", amps);
      break;
    case 2:   // Tariff and supply
      snprintf(l0, sizeof(l0), "Rate Rs%lu/hr", (unsigned long)RATE_RS_PER_HOUR);
      snprintf(l1, sizeof(l1), "Supply: %s", relayState ? "ON" : "OFF");
      break;
    default:  // Hotspot
      snprintf(l0, sizeof(l0), "%s", AP_SSID);
      snprintf(l1, sizeof(l1), "%s C:%d", WiFi.softAPIP().toString().c_str(), (int)WiFi.softAPgetStationNum());
      break;
  }
  drawLine(0, l0);
  drawLine(1, l1);
}

void nextPage() {
  currentPage = (currentPage + 1) % NUM_PAGES;
  flashUntil = 0;
  drawPage();
  Serial.printf("LCD page %d\n", currentPage + 1);
}

void doReset() {
  Serial.println("Long press: restarting...");
  saveState();
  digitalWrite(RELAY_PIN, RELAY_OFF);
  digitalWrite(BUZZER_PIN, BUZZER_OFF);
  lcd.clear();
  drawLine(0, "Restarting...");
  delay(700);
  ESP.restart();
}

// ---------- Switch: short press = next page, 3 s hold = restart ----------
void handleButton() {
  bool reading = digitalRead(SWITCH_PIN);

  if (reading != btnLastRead) {
    btnLastRead = reading;
    btnLastChange = millis();
  }

  if (millis() - btnLastChange >= DEBOUNCE_MS && reading != btnStable) {
    btnStable = reading;
    if (btnStable == LOW) {            // pressed
      btnPressedAt = millis();
      longHandled = false;
    } else {                           // released
      if (!longHandled) nextPage();    // short press
    }
  }

  if (btnStable == LOW && !longHandled && millis() - btnPressedAt >= LONG_PRESS_MS) {
    longHandled = true;
    doReset();
  }
}

// ======================= PREPAID LOGIC =======================

void doRecharge(uint32_t amountRs) {
  uint32_t add = (uint32_t)(((uint64_t)amountRs * 3600ULL) / RATE_RS_PER_HOUR);
  if (add < 1) add = 1;

  bool wasEmpty = (remaining == 0);
  remaining += add;
  if (remaining > MAX_SECONDS) remaining = MAX_SECONDS;

  if (wasEmpty) {
    running = true;          // supply starts automatically after a recharge from zero
    capSec = remaining;
  } else if (remaining > capSec) {
    capSec = remaining;
  }

  totalRs += amountRs;
  recharges++;

  applyRelay();
  saveState();
  startBeeps(2);

  char a[20], b[20];
  snprintf(a, sizeof(a), "Added Rs %lu", (unsigned long)amountRs);
  snprintf(b, sizeof(b), "Left %lu:%02lu:%02lu",
           (unsigned long)(remaining / 3600),
           (unsigned long)((remaining % 3600) / 60),
           (unsigned long)(remaining % 60));
  setFlash(a, b, 3000);
  drawPage();
  Serial.printf("Recharge Rs %lu -> %lu s left\n", (unsigned long)amountRs, (unsigned long)remaining);
}

void onExpired() {
  running = false;
  applyRelay();
  saveState();
  startBeeps(6);
  Serial.println("Balance finished. Supply cut off.");
}

// Called once per second
void tickSecond() {
  if (running && remaining > 0) {
    remaining--;
    energyWh += watts / 3600.0f;

    if (remaining == 0) {
      onExpired();
    } else {
      if (remaining == LOW_WARN_SEC) startBeeps(3);
      else if (remaining < LOW_WARN_SEC && remaining % 60 == 0) startBeeps(1);
      if (remaining % SAVE_EVERY_S == 0) saveState();
    }
  }
  drawPage();
}

// ======================= WEB =======================

const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Prepaid Meter</title>
<style>
:root{--bg:#0b1220;--card:#131c2e;--line:#22304d;--text:#e8edf7;--mute:#8b98b5;--ok:#22c55e;--warn:#f59e0b;--bad:#ef4444;--acc:#38bdf8}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
.wrap{max-width:720px;margin:0 auto;padding:16px 16px 48px}
header{display:flex;justify-content:space-between;align-items:center;margin-bottom:14px}
h1{font-size:18px;margin:0}
.sub{font-size:12px;color:var(--mute);margin-top:2px}
.pill{padding:6px 12px;border-radius:999px;font-size:12px;font-weight:700;letter-spacing:.06em}
.pill.RUNNING{background:#14532d;color:#86efac}
.pill.PAUSED{background:#78350f;color:#fcd34d}
.pill.EXPIRED{background:#7f1d1d;color:#fca5a5}
.card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:16px;margin-bottom:12px}
.card h2{font-size:13px;color:var(--mute);margin:0 0 10px;font-weight:600;text-transform:uppercase;letter-spacing:.06em}
.ring{position:relative;width:200px;height:200px;margin:4px auto 8px}
.ring svg{width:100%;height:100%;transform:rotate(-90deg)}
.ring .bg{fill:none;stroke:var(--line);stroke-width:12}
.ring .fg{fill:none;stroke:var(--ok);stroke-width:12;stroke-linecap:round;transition:stroke-dashoffset .6s,stroke .3s}
.ring .mid{position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;justify-content:center}
.big{font-size:30px;font-weight:700;font-variant-numeric:tabular-nums}
.mute{color:var(--mute);font-size:12px}
.bal{text-align:center;font-size:20px;font-weight:600;margin-bottom:12px}
.banner{border-radius:12px;padding:10px 12px;font-size:14px;margin-bottom:12px}
.banner.warn{background:#78350f;color:#fde68a}
.banner.bad{background:#7f1d1d;color:#fecaca}
button{border:0;border-radius:12px;padding:13px 16px;font-size:15px;font-weight:600;color:#fff;cursor:pointer;width:100%}
button:disabled{opacity:.45;cursor:not-allowed}
.primary{background:#2563eb}
.ghost{background:#1e293b;border:1px solid var(--line)}
.danger{background:#b91c1c}
.chips{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:10px}
.chip{background:#1e293b;border:1px solid var(--line);color:var(--text);width:auto;padding:9px 14px;border-radius:999px;font-size:14px}
.chip:active{background:#334155}
.row{display:flex;gap:8px;margin-bottom:8px}
input{flex:1;min-width:0;background:#0b1220;border:1px solid var(--line);border-radius:12px;color:var(--text);padding:12px;font-size:16px}
.prev{font-size:13px;color:var(--acc);margin:2px 0 10px;min-height:18px}
.grid3{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}
.stat{background:#0f1729;border-radius:12px;padding:10px;text-align:center}
.stat b{display:block;font-size:20px;font-variant-numeric:tabular-nums}
.stat span{font-size:11px;color:var(--mute)}
canvas{width:100%;height:140px;display:block}
.legend{font-size:12px;color:var(--mute);margin-top:6px}
.legend i{display:inline-block;width:10px;height:10px;border-radius:50%;margin:0 4px 0 10px}
.kv{display:flex;justify-content:space-between;padding:7px 0;border-bottom:1px solid var(--line);font-size:14px}
.kv:last-child{border:0}
.kv span:first-child{color:var(--mute)}
details summary{cursor:pointer;color:var(--mute);font-size:13px}
.toast{position:fixed;left:50%;bottom:20px;transform:translate(-50%,40px);opacity:0;padding:12px 18px;border-radius:12px;font-size:14px;font-weight:600;transition:.25s;pointer-events:none;max-width:90%}
.toast.show{opacity:1;transform:translate(-50%,0)}
.toast.good{background:#14532d;color:#bbf7d0}
.toast.bad{background:#7f1d1d;color:#fecaca}
</style>
</head>
<body>
<div class="wrap">
  <header>
    <div><h1>Prepaid Power Meter</h1><div class="sub" id="conn">Connecting...</div></div>
    <div class="pill PAUSED" id="pill">--</div>
  </header>

  <div class="banner" id="banner" hidden></div>

  <div class="card">
    <div class="ring">
      <svg viewBox="0 0 160 160"><circle class="bg" cx="80" cy="80" r="70"></circle><circle class="fg" id="fg" cx="80" cy="80" r="70"></circle></svg>
      <div class="mid"><div class="big" id="time">00:00:00</div><div class="mute">time left</div></div>
    </div>
    <div class="bal" id="bal">Rs 0.00</div>
    <button class="ghost" id="toggle">Start supply</button>
  </div>

  <div class="card">
    <h2>Recharge</h2>
    <div class="chips">
      <button class="chip" data-a="10">Rs 10</button>
      <button class="chip" data-a="50">Rs 50</button>
      <button class="chip" data-a="100">Rs 100</button>
      <button class="chip" data-a="200">Rs 200</button>
      <button class="chip" data-a="500">Rs 500</button>
    </div>
    <div class="row">
      <input id="amt" type="number" inputmode="numeric" min="1" placeholder="Amount in Rs">
      <input id="pin" type="password" inputmode="numeric" placeholder="PIN" autocomplete="off">
    </div>
    <div class="prev" id="prev"></div>
    <button class="primary" id="go">Recharge</button>
  </div>

  <div class="card">
    <h2>Live readings</h2>
    <div class="grid3">
      <div class="stat"><b id="v">--</b><span>Volts</span></div>
      <div class="stat"><b id="i">--</b><span>Amps</span></div>
      <div class="stat"><b id="w">--</b><span>Watts</span></div>
    </div>
    <canvas id="cv"></canvas>
    <div class="legend"><i style="background:#38bdf8"></i>Voltage<i style="background:#f59e0b"></i>Current</div>
  </div>

  <div class="card">
    <h2>Details</h2>
    <div class="kv"><span>Supply (relay)</span><span id="supply">--</span></div>
    <div class="kv"><span>Tariff</span><span id="rate">--</span></div>
    <div class="kv"><span>Energy this session</span><span id="wh">--</span></div>
    <div class="kv"><span>Total recharged</span><span id="tot">--</span></div>
    <div class="kv"><span>Devices connected</span><span id="cl">--</span></div>
  </div>

  <details class="card">
    <summary>Operator tools</summary>
    <p class="mute">Uses the PIN typed in the recharge box above.</p>
    <button class="danger" id="clr">Clear remaining balance</button>
  </details>
</div>
<div class="toast" id="toast"></div>

<script>
const $=id=>document.getElementById(id);
const CIRC=2*Math.PI*70;
const N=60;
let S=null;const hv=[],hi=[];

function pad(n){return String(n).padStart(2,'0');}
function fmt(s){return pad(Math.floor(s/3600))+':'+pad(Math.floor(s%3600/60))+':'+pad(s%60);}
function human(s){
  if(s<60)return s+' s';
  const h=Math.floor(s/3600),m=Math.round(s%3600/60);
  if(m===60)return (h+1)+' h';
  return ((h?h+' h ':'')+(m?m+' min':'')).trim();
}
let tt;
function toast(msg,ok){
  const t=$('toast');t.textContent=msg;t.className='toast show '+(ok===false?'bad':'good');
  clearTimeout(tt);tt=setTimeout(()=>{t.className='toast';},2600);
}
async function post(url,data){
  try{const r=await fetch(url,{method:'POST',body:new URLSearchParams(data||{})});return await r.json();}
  catch(e){return {ok:false,msg:'No connection to the meter'};}
}
async function poll(){
  try{
    const r=await fetch('/api/state',{cache:'no-store'});
    S=await r.json();
    $('conn').textContent='Connected';
    render();
  }catch(e){$('conn').textContent='Reconnecting...';}
}
function preview(){
  const a=parseInt($('amt').value,10);
  $('prev').textContent=(S&&a>0)?('Rs '+a+' gives '+human(Math.round(a*3600/S.rate))):'Choose or type an amount';
}
function line(g,a,max,col,w,h){
  if(a.length<2)return;
  g.strokeStyle=col;g.lineWidth=2;g.beginPath();
  a.forEach((v,k)=>{
    const x=k*(w/(N-1)),y=h-4-(v/max)*(h-8);
    if(k)g.lineTo(x,y);else g.moveTo(x,y);
  });
  g.stroke();
}
function draw(){
  const cv=$('cv'),dpr=window.devicePixelRatio||1;
  const w=cv.clientWidth,h=cv.clientHeight;
  if(!w)return;
  cv.width=w*dpr;cv.height=h*dpr;
  const g=cv.getContext('2d');
  g.scale(dpr,dpr);g.clearRect(0,0,w,h);
  g.strokeStyle='#22304d';g.lineWidth=1;
  for(let k=1;k<4;k++){const y=h*k/4;g.beginPath();g.moveTo(0,y);g.lineTo(w,y);g.stroke();}
  line(g,hv,Math.max(260,...hv),'#38bdf8',w,h);
  line(g,hi,Math.max(1,...hi),'#f59e0b',w,h);
}
function render(){
  const s=S;
  const pill=$('pill');pill.textContent=s.state;pill.className='pill '+s.state;
  $('time').textContent=fmt(s.rem);
  $('bal').textContent='Rs '+s.bal.toFixed(2);
  const frac=s.cap>0?Math.min(1,s.rem/s.cap):0;
  const fg=$('fg');
  fg.style.strokeDasharray=CIRC;
  fg.style.strokeDashoffset=CIRC*(1-frac);
  fg.style.stroke=s.state==='EXPIRED'?'var(--bad)':(s.low?'var(--warn)':'var(--ok)');
  const b=$('banner');
  if(s.state==='EXPIRED'){b.hidden=false;b.className='banner bad';b.textContent='Time is over. Recharge to restore the supply.';}
  else if(s.low){b.hidden=false;b.className='banner warn';b.textContent='Low balance: '+human(s.rem)+' left. Recharge soon.';}
  else{b.hidden=true;}
  const tg=$('toggle');
  tg.textContent=s.state==='RUNNING'?'Pause supply':'Start supply';
  tg.disabled=(s.rem===0);
  $('v').textContent=s.volts.toFixed(1);
  $('i').textContent=s.amps.toFixed(2);
  $('w').textContent=s.watts.toFixed(0);
  $('supply').textContent=s.supply?'ON':'OFF';
  $('rate').textContent='Rs '+s.rate+' per hour';
  $('wh').textContent=s.wh.toFixed(2)+' Wh';
  $('tot').textContent='Rs '+s.tot+' ('+s.cnt+' recharges)';
  $('cl').textContent=s.clients;
  hv.push(s.volts);hi.push(s.amps);
  if(hv.length>N){hv.shift();hi.shift();}
  draw();preview();
}

$('toggle').onclick=async()=>{const j=await post('/api/toggle');toast(j.msg,j.ok);poll();};
$('go').onclick=async()=>{
  const j=await post('/api/recharge',{amount:$('amt').value,pin:$('pin').value});
  toast(j.msg,j.ok);
  if(j.ok){$('amt').value='';}
  poll();
};
$('clr').onclick=async()=>{
  if(!confirm('Clear the remaining balance?'))return;
  const j=await post('/api/clear',{pin:$('pin').value});
  toast(j.msg,j.ok);poll();
};
document.querySelectorAll('.chip').forEach(c=>{c.onclick=()=>{$('amt').value=c.dataset.a;preview();};});
$('amt').oninput=preview;
window.addEventListener('resize',draw);
poll();setInterval(poll,1000);
</script>
</body>
</html>
)rawliteral";

void sendJson(int code, const char* msg, bool ok) {
  char b[120];
  snprintf(b, sizeof(b), "{\"ok\":%s,\"msg\":\"%s\"}", ok ? "true" : "false", msg);
  server.send(code, "application/json", b);
}

bool pinOk() {
  return strlen(ADMIN_PIN) == 0 || server.arg("pin") == ADMIN_PIN;
}

void handleRoot() {
  server.send_P(200, "text/html", PAGE);
}

void handleState() {
  const char* st = (remaining == 0) ? "EXPIRED" : (running ? "RUNNING" : "PAUSED");
  bool low = remaining > 0 && remaining <= LOW_WARN_SEC;
  char b[560];
  snprintf(b, sizeof(b),
           "{\"rem\":%lu,\"cap\":%lu,\"rate\":%lu,\"bal\":%.2f,\"state\":\"%s\",\"supply\":%d,\"low\":%s,"
           "\"volts\":%.1f,\"amps\":%.2f,\"watts\":%.0f,\"wh\":%.2f,"
           "\"tot\":%lu,\"cnt\":%lu,\"clients\":%d}",
           (unsigned long)remaining, (unsigned long)capSec, (unsigned long)RATE_RS_PER_HOUR,
           balanceRs(), st, relayState ? 1 : 0, low ? "true" : "false",
           volts, amps, watts, energyWh,
           (unsigned long)totalRs, (unsigned long)recharges, (int)WiFi.softAPgetStationNum());
  server.send(200, "application/json", b);
}

void handleRecharge() {
  if (!pinOk()) { sendJson(403, "Wrong PIN", false); return; }
  long amount = server.arg("amount").toInt();
  if (amount < 1 || amount > (long)MAX_RECHARGE_RS) {
    sendJson(400, "Enter an amount from 1 to 5000", false);
    return;
  }
  doRecharge((uint32_t)amount);
  char m[40];
  snprintf(m, sizeof(m), "Recharged Rs %ld", amount);
  sendJson(200, m, true);
}

void handleToggle() {
  if (remaining == 0) { sendJson(400, "No balance. Recharge first", false); return; }
  running = !running;
  applyRelay();
  saveState();
  drawPage();
  sendJson(200, running ? "Supply started" : "Supply paused", true);
}

void handleClear() {
  if (!pinOk()) { sendJson(403, "Wrong PIN", false); return; }
  remaining = 0;
  capSec = 0;
  running = false;
  applyRelay();
  saveState();
  drawPage();
  sendJson(200, "Balance cleared", true);
}

// Send every unknown address (phone connectivity checks etc.) to the dashboard
void handleNotFound() {
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  server.send(302, "text/plain", "");
}

// ======================= SETUP / LOOP =======================

void setup() {
  Serial.begin(115200);

  // Set output levels BEFORE enabling the outputs, so the relay does not click at boot
  digitalWrite(RELAY_PIN, RELAY_OFF);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, BUZZER_OFF);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, BUZZER_OFF);

  pinMode(SWITCH_PIN, INPUT_PULLUP);
  btnLastRead = btnStable = digitalRead(SWITCH_PIN);

  analogReadResolution(12);
  analogSetPinAttenuation(CURRENT_PIN, ADC_11db);
  analogSetPinAttenuation(VOLT_PIN, ADC_11db);

  Wire.begin(LCD_SDA, LCD_SCL);
  lcd.init();
  lcd.backlight();
  drawLine(0, "Prepaid Meter");
  drawLine(1, "Starting...");

  prefs.begin("prepaid", false);
  zeroCurrent();      // relay is still off here
  loadState();
  applyRelay();       // resumes the supply if time is left and it was running

  // Hotspot
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, strlen(AP_PASS) ? AP_PASS : nullptr);
  delay(200);
  dnsServer.start(53, "*", WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/api/state", handleState);
  server.on("/api/recharge", HTTP_POST, handleRecharge);
  server.on("/api/toggle", HTTP_POST, handleToggle);
  server.on("/api/clear", HTTP_POST, handleClear);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.printf("Hotspot: %s  Password: %s\n", AP_SSID, AP_PASS);
  Serial.print("Dashboard: http://");
  Serial.println(WiFi.softAPIP());

  setFlash(AP_SSID, WiFi.softAPIP().toString().c_str(), 4000);
  drawPage();
  lastTick = millis();

  Serial.println("Switch: short press = next LCD page, hold 3 s = restart");
  Serial.println("Serial: z = zero current sensor, + = add Rs 10 (test)");
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  handleButton();
  updateBuzzer();

  unsigned long now = millis();

  // One-second countdown
  if (now - lastTick >= 1000) {
    lastTick += 1000;
    if (now - lastTick > 3000) lastTick = now;   // catch up after a long stall
    tickSecond();
  }

  // Measure every 500 ms
  if (now - lastMeasure >= 500) {
    lastMeasure = now;

    vPinMv = readAvgMv(VOLT_PIN);
    vin = vPinMv / 1000.0f * VOLT_DIVIDER_RATIO;
    volts = vin * (230.0f / SIM_INPUT_FOR_230V);

    acsMean = readAvgMv(CURRENT_PIN);
    amps = (acsMean - acsZero) / ACS_DIVIDER / ACS_MV_PER_A;
    if (fabsf(amps) < 0.03f) amps = 0;

    watts = volts * fabsf(amps);
  }

  // Status on the Serial Monitor every 5 s
  if (now - lastPrint >= 5000) {
    lastPrint = now;
    Serial.printf("Left %lu s | Rs %.2f | %s | Relay %s | V %.1f | I %.2f A | clients %d\n",
                  (unsigned long)remaining, balanceRs(), running ? "RUN" : "HOLD",
                  relayState ? "ON" : "OFF", volts, amps, (int)WiFi.softAPgetStationNum());
  }

  // Serial commands
  while (Serial.available()) {
    char cmd = Serial.read();
    if (cmd == 'z') zeroCurrent();
    else if (cmd == '+') doRecharge(10);
  }
}
