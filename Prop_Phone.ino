#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <SoftwareSerial.h>
#include "DFRobotDFPlayerMini.h"

// ================== RING OUTPUT MODE ==================
// 0 = piezo/buzzer on D8 (tone() two-tone)           -> low power test
// 1 = low-voltage transistor on D8 (PWM on/off)      -> e.g., 12 V coil
// 2 = RELAY module on D8, switches 12–50 V ringer    -> safest for 50 V
#define RING_MODE_PIEZO         0
#define RING_MODE_TRANSISTOR    1
#define RING_MODE_RELAY_HV      2
#define RING_MODE               RING_MODE_RELAY_HV   // <<< pick one

// For piezo mode: true for passive piezo disc on D8 (tone), false for active buzzer
const bool USE_PASSIVE_PIEZO = true; // ignored in transistor/relay modes

// ===== Wi-Fi =====
const char* SSID = "NETGEAR";
const char* PASS = "pass";

// ===== Pins =====
const int PIN_HOOK  = D7;  // Hook switch -> GND (INPUT_PULLUP)
const int PIN_RING  = D8;  // Ring output (piezo OR transistor base via 1k OR relay IN)
const int PIN_DF_RX = D5;  // ESP TX -> DF RX
const int PIN_DF_TX = D6;  // DF TX -> ESP RX (optional)

// Rotary dial (pulse pair & optional off-normal)
const int PIN_DIAL_PULSE = D2; // dial pulse contact (other side to GND)
const int PIN_DIAL_ON    = D1; // optional off-normal (other side to GND)

// ===== LED =====
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif
inline void ledOn()   { digitalWrite(LED_BUILTIN, LOW); }
inline void ledOff()  { digitalWrite(LED_BUILTIN, HIGH); }
inline bool ledIsOn() { return digitalRead(LED_BUILTIN) == LOW; }

// ===== DFPlayer =====
SoftwareSerial mp3Serial(PIN_DF_TX, PIN_DF_RX);
DFRobotDFPlayerMini dfp;

// ===== Web =====
ESP8266WebServer server(80);

// ===== State =====
bool ringing     = false;
bool offHook     = false;
int  queuedTrack = 1;
int  currentVol  = 24;
bool defaultLoop = true;   // loop track 1 @ vol 12 when off-hook if user didn't choose a track
int  totalTracks = 0;      // dynamic track count

// ===== Ring cadence =====
const unsigned long CADENCE_ON  = 2000; // 2 s
const unsigned long CADENCE_OFF = 4000; // 4 s
unsigned long ringPhaseStart = 0;
bool ringOnWindow = false;

// ===== Ring vars =====
const unsigned long TRILL_MS = 50;
unsigned long lastTrill = 0;
bool use440 = true;

uint16_t ringHz   = 20;   // adjustable via slider (5..40)
uint8_t  ringDuty = 55;   // fixed duty for PWM on D8
unsigned long cycleStart = 0;

// ===== Rotary dial state =====
volatile uint32_t dialLastEdgeMs = 0;
volatile uint16_t dialPulseCount = 0;
volatile bool dialTick = false;     // ISR sets when a pulse edge counted
volatile bool dialActive = false;   // optional off-normal
uint32_t lastPulseSeenMs = 0;
uint8_t  lastDigit = 0;
String   dialBuffer = "";           // digits since last clear

const uint16_t DIAL_DEBOUNCE_MS   = 4;    // ignore edges faster than this
const uint16_t DIAL_INTERDIGIT_MS = 200;  // gap that marks digit end

// ====== IP LED announcer (blink last octet) ======
bool ipAnnounce = false;     // true after WiFi connect; disabled when user touches LED controls
uint8_t ipDigits[3];         // up to 3 digits
uint8_t ipDigitsLen = 0;
uint8_t ipDigitIndex = 0;    // which digit we are on
uint8_t ipBlinkCount = 0;    // how many blinks completed in current digit
bool    ipBlinkOn = false;   // LED on/off state within a blink
uint8_t ipPhase = 0;         // 0=blinking digit, 1=digit gap, 2=cycle gap
unsigned long ipTimer = 0;

const unsigned long IP_BLINK_ON_MS     = 140;  // on time for each blink
const unsigned long IP_BLINK_OFF_MS    = 160;  // off time between blinks
const unsigned long IP_DIGIT_GAP_MS    = 600;  // gap between digits
const unsigned long IP_CYCLE_GAP_MS    = 1300; // gap between full repeats

void setIpDigitsFromIP(IPAddress ip) {
  uint8_t last = ip[3];
  String s = String(last);
  ipDigitsLen = s.length();
  if (last == 0) {
    ipDigits[0] = 10; // represent zero as 10 blinks
    ipDigitsLen = 1;
  } else {
    for (uint8_t i=0; i<ipDigitsLen; i++) {
      uint8_t d = s.charAt(i) - '0';
      ipDigits[i] = (d == 0) ? 10 : d;
    }
  }
  ipDigitIndex = 0;
  ipBlinkCount = 0;
  ipBlinkOn = false;
  ipPhase = 0;
  ipTimer = millis();
}

void ipAnnounceLogic() {
  if (!ipAnnounce) return;
  unsigned long now = millis();
  switch (ipPhase) {
    case 0: {
      uint8_t targetBlinks = ipDigits[ipDigitIndex];
      if (!ipBlinkOn) {
        if (now - ipTimer >= IP_BLINK_OFF_MS || ipBlinkCount == 0) {
          ledOn();
          ipBlinkOn = true; ipTimer = now;
        }
      } else {
        if (now - ipTimer >= IP_BLINK_ON_MS) {
          ledOff();
          ipBlinkOn = false; ipTimer = now; ipBlinkCount++;
          if (ipBlinkCount >= targetBlinks) { ipPhase = 1; ipBlinkCount = 0; }
        }
      }
    } break;
    case 1: {
      if (now - ipTimer >= IP_DIGIT_GAP_MS) {
        ipTimer = now; ipDigitIndex++;
        ipPhase = (ipDigitIndex >= ipDigitsLen) ? 2 : 0;
        if (ipPhase == 2) ipDigitIndex = 0;
      }
    } break;
    case 2: {
      if (now - ipTimer >= IP_CYCLE_GAP_MS) { ipTimer = now; ipPhase = 0; }
    } break;
  }
}

// ===== Helpers =====
void dfSetVolume(int v) {
  v = constrain(v, 0, 30);
  currentVol = v;
  dfp.volume(v);
  Serial.print(F("[DFP] Volume=")); Serial.println(currentVol);
}

// Smart play policy for requested track:
// 1..3 => loop; 4..5 => play once; >5 => play once.
void playTrackSmart(int n) {
  n = max(1, n);
  dfSetVolume(currentVol);
  if (n >= 1 && n <= 3) {
    dfp.loop(n);
    Serial.print(F("[DFP] loop track ")); Serial.println(n);
  } else {
    dfp.play(n);
    Serial.print(F("[DFP] play track ")); Serial.println(n);
  }
  queuedTrack = n;
  defaultLoop = false;
}

void piezoOn(unsigned freqHz = 1000) {
#if (RING_MODE == RING_MODE_PIEZO)
  if (USE_PASSIVE_PIEZO) tone(PIN_RING, freqHz);
  else                   digitalWrite(PIN_RING, HIGH);
#endif
}
void piezoOff() {
#if (RING_MODE == RING_MODE_PIEZO)
  if (USE_PASSIVE_PIEZO) noTone(PIN_RING);
  else                   digitalWrite(PIN_RING, LOW);
#endif
}

// ===== Track count =====
int refreshTrackCount() {
  delay(50);
  int16_t c = dfp.readFileCounts();   // total files in root
  if (c < 0) { delay(120); c = dfp.readFileCounts(); }
  if (c < 0) c = 0;
  totalTracks = c;
  Serial.print(F("[DFP] Total tracks: ")); Serial.println(totalTracks);
  return totalTracks;
}

// ===== Rotary dial ISRs =====
ICACHE_RAM_ATTR void dialPulseISR() {
  uint32_t now = millis();
  if (now - dialLastEdgeMs < DIAL_DEBOUNCE_MS) return; // debounce
  dialLastEdgeMs = now;
  if (digitalRead(PIN_DIAL_PULSE) == LOW) {
    dialPulseCount++;
    dialTick = true;
  }
}
ICACHE_RAM_ATTR void dialOnISR() {
  dialActive = (digitalRead(PIN_DIAL_ON) == LOW);
}

// ===== Ring driver =====
void ringLogic() {
  unsigned long now = millis();

  if (!ringing) {
#if (RING_MODE == RING_MODE_PIEZO)
    piezoOff();
#else
    digitalWrite(PIN_RING, LOW); // transistor off or relay released
#endif
    ringOnWindow = false;
    ringPhaseStart = 0;
    return;
  }

  if (ringPhaseStart == 0) {
    ringPhaseStart = now;
    ringOnWindow   = true;
#if (RING_MODE == RING_MODE_PIEZO)
    use440 = true;
    if (USE_PASSIVE_PIEZO) tone(PIN_RING, 440); else digitalWrite(PIN_RING, HIGH);
    lastTrill = now;
#else
    cycleStart = now;
#endif
    Serial.println(F("[RING] ON window start"));
  }

  if (ringOnWindow) {
#if (RING_MODE == RING_MODE_PIEZO)
    if (USE_PASSIVE_PIEZO && (now - lastTrill >= TRILL_MS)) {
      use440 = !use440;
      tone(PIN_RING, use440 ? 440 : 480);
      lastTrill = now;
    }
#else
    // PWM-ish on/off on D8 (for transistor or relay input). Relay will "buzz" at low Hz;
    // typical relay modules click fine at ~20 Hz. If you prefer no buzz, set ringHz=1..2.
    uint16_t hz = constrain(ringHz, 1, 40);
    uint8_t  du = constrain(ringDuty, 5, 95);
    unsigned long period = max(5UL, 1000UL / hz);
    unsigned long onMs   = (period * du) / 100;
    unsigned long t      = now - cycleStart;
    digitalWrite(PIN_RING, (t < onMs) ? HIGH : LOW);
    if (t >= period) {
      cycleStart += period;
      if (now - cycleStart > period * 2) cycleStart = now;
    }
#endif
    if (now - ringPhaseStart >= CADENCE_ON) {
#if (RING_MODE == RING_MODE_PIEZO)
      piezoOff();
#else
      digitalWrite(PIN_RING, LOW);
#endif
      ringOnWindow = false;
      ringPhaseStart = now;
      Serial.println(F("[RING] OFF window"));
    }
  } else {
    if (now - ringPhaseStart >= CADENCE_OFF) {
      ringPhaseStart = now;
      ringOnWindow   = true;
#if (RING_MODE == RING_MODE_PIEZO)
      use440 = true;
      if (USE_PASSIVE_PIEZO) tone(PIN_RING, 440); else digitalWrite(PIN_RING, HIGH);
      lastTrill = now;
#else
      cycleStart = now;
#endif
      Serial.println(F("[RING] ON window start"));
    }
  }
}

// ====== MOBILE-FRIENDLY PAGE ======
const char PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html>
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>Old Phone Prop</title>
<style>
  :root{--bg:#fff;--fg:#111;--muted:#666;--card:#f6f6f7;--line:#e3e3e7;--acc:#2b7bff;}
  @media (prefers-color-scheme: dark){
    :root{--bg:#0c0d10;--fg:#f3f5f7;--muted:#9aa3ad;--card:#14161b;--line:#262a33;--acc:#4c8dff;}
  }
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.45 system-ui,Segoe UI,Inter,Roboto,sans-serif;padding:16px}
  header{position:sticky;top:0;background:var(--bg);padding:10px 4px 8px;z-index:5;border-bottom:1px solid var(--line);margin:-16px -16px 16px}
  h1{font-size:20px;margin:0 12px}
  .row{display:flex;align-items:center;gap:8px;padding:0 12px}
  .badge{font-size:12px;padding:2px 8px;border:1px solid var(--line);border-radius:999px;color:var(--muted)}
  .grid{display:grid;gap:12px}
  @media(min-width:640px){ .grid{grid-template-columns:repeat(2,1fr)} }
  @media(min-width:960px){ .grid{grid-template-columns:repeat(3,1fr)} }
  .card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px}
  .title{font-size:16px;font-weight:600;margin:0 0 10px}
  .btnbar{display:flex;flex-wrap:wrap;gap:8px}
  button{appearance:none;border:1px solid var(--line);background:#fff0;border-radius:12px;padding:10px 14px;color:var(--fg);font-weight:600;min-width:120px}
  button:hover{border-color:var(--acc)}
  .primary{background:var(--acc);color:#fff;border-color:var(--acc)}
  .danger{border-color:#ff5a52;color:#ff5a52}
  .ok{border-color:#28a745;color:#28a745}
  label{display:block;font-size:13px;color:var(--muted);margin-bottom:6px}
  input[type=range]{width:100%}
  .pill{display:inline-block;padding:2px 8px;border-radius:999px;border:1px solid var(--line);font-size:12px}
  .muted{color:var(--muted)}
  .sp{height:6px}
  .hint{font-size:12px;color:var(--muted)}
</style>

<header class="row">
  <h1>Old Phone Prop</h1>
  <span class="badge" id="ip">IP: …</span>
  <span class="badge" id="mode">Mode: …</span>
</header>

<div class="grid">
  <section class="card">
    <p class="title">Status</p>
    <div><b>Hook:</b> <span id="hook" class="pill">—</span></div>
    <div><b>Ringing:</b> <span id="ringing" class="pill">—</span></div>
    <div><b>LED:</b> <span id="led" class="pill">—</span></div>
    <div><b>Tracks:</b> <span id="trackcount" class="pill">—</span></div>
    <div><b>Last digit:</b> <span id="lastd" class="pill">—</span></div>
    <div><b>Buffer:</b> <span id="buf" class="pill">—</span></div>
    <div class="btnbar" style="margin-top:8px">
      <button onclick="api('/cleardial')">Clear Buffer</button>
    </div>
  </section>

  <section class="card">
    <p class="title">Phone Control</p>
    <div class="btnbar">
      <button class="primary" onclick="api('/ring')">Ring</button>
      <button onclick="api('/stopring')">Stop Ring</button>
      <button onclick="api('/beep')">Beep</button>
      <button class="danger" onclick="api('/stopplay')">Stop Playing</button>
      <button onclick="rescan()" title="Re-read SD card">Rescan SD</button>
    </div>
    <div class="sp"></div>
    <label>Ring frequency (Hz) <span class="muted" id="hzv"></span></label>
    <input id="hz" type="range" min="1" max="40" value="20" oninput="hzv.textContent=this.value" onchange="api('/sethz?hz='+this.value)">
  </section>

  <section class="card">
    <p class="title">Volume</p>
    <label>DFPlayer volume (0–30): <span id="volv" class="muted"></span></label>
    <input id="vol" type="range" min="0" max="30" value="24" oninput="volv.textContent=this.value" onchange="api('/volume?level='+this.value)">
    <div class="btnbar" style="margin-top:8px">
      <button onclick="setVol(8)">Quiet (8)</button>
      <button onclick="setVol(12)">Default loop (12)</button>
      <button onclick="setVol(20)">Loud (20)</button>
      <button onclick="setVol(28)">Max-ish (28)</button>
    </div>
  </section>

  <section class="card">
    <p class="title">LED</p>
    <div class="btnbar">
      <button class="ok" onclick="api('/led/on')">ON</button>
      <button class="danger" onclick="api('/led/off')">OFF</button>
      <button onclick="api('/led/toggle')">TOGGLE</button>
    </div>
    <p class="hint" style="margin-top:8px">At boot the LED blinks your IP’s last octet. Any LED action here disables that announcer.</p>
  </section>

  <section class="card" style="grid-column:1/-1">
    <p class="title">Tracks</p>
    <div class="btnbar" id="named">
      <button onclick="api('/play?id=1')">Dial (loop)</button>
      <button onclick="api('/play?id=2')">Calling (loop)</button>
      <button onclick="api('/play?id=3')">End Call (loop)</button>
      <button onclick="api('/play?id=4')">Voicemail (once)</button>
      <button onclick="api('/play?id=5')">Number doesn’t exist (once)</button>
    </div>
    <p class="hint">Buttons above assume SD tracks 1..5 map to these sounds.</p>
    <div class="btnbar" id="tracks"><span class="muted">Loading others…</span></div>
  </section>
</div>

<script>
  const $ = s=>document.querySelector(s);
  const ip=$('#ip'), mode=$('#mode'), hook=$('#hook'), ring=$('#ringing'), led=$('#led'), tc=$('#trackcount');
  const hz=$('#hz'), hzv=$('#hzv'), vol=$('#vol'), volv=$('#volv'), tracks=$('#tracks');
  const lastd=$('#lastd'), buf=$('#buf');

  async function api(path){ try{ await fetch(path); }catch(e){} setTimeout(refresh,120); }
  function setVol(v){ vol.value=v; volv.textContent=v; api('/volume?level='+v); }
  async function rescan(){ await api('/rescan'); await buildTracks(); }

  async function buildTracks(){
    try{
      const r = await fetch('/trackcount'); const n = parseInt(await r.text())||0;
      tc.textContent = n;
      if (n <= 5){ tracks.innerHTML = '<span class="muted">No extra tracks.</span>'; return; }
      let h = '';
      for (let i=6;i<=n;i++){ h += `<button onclick="api('/play?id=${i}')">Track ${i}</button>`; }
      tracks.innerHTML = h;
    }catch(e){
      tracks.innerHTML = '<span class="muted">Error loading tracks.</span>';
    }
  }

  async function refresh(){
    try{
      const r = await fetch('/state'); const s = await r.json();
      ip.textContent   = 'IP: ' + s.ip;
      mode.textContent = 'Mode: ' + s.mode;
      hook.textContent = s.hook ? 'OFF-HOOK' : 'ON-HOOK';
      ring.textContent = s.ringing ? 'YES' : 'NO';
      led.textContent  = s.led ? 'ON' : 'OFF';
      tc.textContent   = s.totalTracks;
      hz.value = s.ringHz; hzv.textContent = s.ringHz;
      vol.value = s.currentVol; volv.textContent = s.currentVol;
      lastd.textContent = (s.lastDigit !== undefined) ? s.lastDigit : '—';
      buf.textContent   = (s.dialBuffer !== undefined) ? s.dialBuffer : '—';
    }catch(e){}
  }

  buildTracks();
  refresh();
  setInterval(refresh, 1000);
</script>
</html>
)HTML";

// ===== JSON /state =====
String jsonState() {
  String mode =
#if (RING_MODE == RING_MODE_PIEZO)
    (USE_PASSIVE_PIEZO ? "Piezo (two-tone)" : "Active buzzer");
#elif (RING_MODE == RING_MODE_TRANSISTOR)
    "Transistor (low voltage)";
#else
    "Relay HV (12–50 V)";
#endif

  String s = "{";
  s += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  s += "\"hook\":" + String(offHook ? "true":"false") + ",";
  s += "\"ringing\":" + String(ringing ? "true":"false") + ",";
  s += "\"led\":" + String(ledIsOn() ? "true":"false") + ",";
  s += "\"ringHz\":" + String(ringHz) + ",";
  s += "\"currentVol\":" + String(currentVol) + ",";
  s += "\"totalTracks\":" + String(totalTracks) + ",";
  s += "\"defaultLoop\":" + String(defaultLoop ? "true":"false") + ",";
  s += "\"lastDigit\":" + String(lastDigit) + ",";
  s += "\"dialBuffer\":\"" + dialBuffer + "\",";
  s += "\"mode\":\"" + mode + "\"";
  s += "}";
  return s;
}

// ===== HTTP handlers =====
void hRoot()      { server.send_P(200,"text/html", PAGE); }
void hState()     { server.send(200,"application/json", jsonState()); }
void hRing()      { ringing = true;  server.send(200,"text/plain","Ringing"); }
void hStopRing()  { ringing = false;
#if (RING_MODE == RING_MODE_PIEZO)
  piezoOff();
#else
  digitalWrite(PIN_RING, LOW);
#endif
  server.send(200,"text/plain","Stopped ring");
}
void hBeep() {
#if (RING_MODE == RING_MODE_PIEZO)
  if (USE_PASSIVE_PIEZO) { tone(PIN_RING, 1200, 400); }
  else { digitalWrite(PIN_RING, HIGH); delay(400); digitalWrite(PIN_RING, LOW); }
#else
  // 300 ms quick buzz/click
  unsigned long t0 = millis();
  while (millis() - t0 < 300) { digitalWrite(PIN_RING, HIGH); delay(12); digitalWrite(PIN_RING, LOW); delay(13); }
#endif
  server.send(200,"text/plain","Beep");
}
void hPlay() {
  int n = server.hasArg("id") ? server.arg("id").toInt() : 1;
  n = max(1, n);
  if (offHook) playTrackSmart(n);
  else { queuedTrack = n; defaultLoop = false; }
  server.send(200,"text/plain", String("Play ")+n);
}
void hStopPlay()  { dfp.stop(); server.send(200,"text/plain","Stopped"); }
void hVolume()    { int v = server.hasArg("level") ? server.arg("level").toInt() : currentVol; dfSetVolume(v); server.send(200,"text/plain", String("Volume=")+currentVol); }

// LED actions: disable announcer when user touches LED
void hLedOn()     { ipAnnounce=false; ledOn();  server.send(200,"text/plain","ON");  }
void hLedOff()    { ipAnnounce=false; ledOff(); server.send(200,"text/plain","OFF"); }
void hLedToggle() { ipAnnounce=false; ledIsOn()?ledOff():ledOn(); server.send(200,"text/plain", ledIsOn()?"ON":"OFF"); }

void hSetHz()     { if (server.hasArg("hz")) { ringHz = constrain(server.arg("hz").toInt(),1,40); } server.send(200,"text/plain", String("Hz=")+ringHz); }
void hTrackCount(){ server.send(200, "text/plain", String(totalTracks)); }
void hRescan()    { int c = refreshTrackCount(); server.send(200, "text/plain", String(c)); }
void hClearDial() { dialBuffer = ""; lastDigit = 0; server.send(200,"text/plain","cleared"); }

// ===== DF setup =====
void setupDF() {
  mp3Serial.begin(9600);
  if (!dfp.begin(mp3Serial)) {
    Serial.println(F("[DFP] Not found. Check wiring/SD."));
    totalTracks = 0;
    return;
  }
  dfSetVolume(currentVol);
  totalTracks = refreshTrackCount();
}

// ===== Setup =====
void setup() {
  Serial.begin(115200);
  delay(60);
  pinMode(LED_BUILTIN, OUTPUT); ledOff();
  pinMode(PIN_HOOK, INPUT_PULLUP);
  pinMode(PIN_RING, OUTPUT);
#if (RING_MODE == RING_MODE_PIEZO)
  piezoOff();
#else
  digitalWrite(PIN_RING, LOW);
#endif

  // Rotary dial inputs
  pinMode(PIN_DIAL_PULSE, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_DIAL_PULSE), dialPulseISR, CHANGE);
  pinMode(PIN_DIAL_ON, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_DIAL_ON), dialOnISR, CHANGE);

  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PASS);
  Serial.print(F("[WiFi] Connecting"));
  while (WiFi.status()!=WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.println(); Serial.print(F("[WiFi] IP: ")); Serial.println(WiFi.localIP());

  // Start IP announcer (blink last octet) until user uses LED controls
  setIpDigitsFromIP(WiFi.localIP()); ipAnnounce = true;

  setupDF();

  server.on("/",            hRoot);
  server.on("/state",       hState);
  server.on("/ring",        hRing);
  server.on("/stopring",    hStopRing);
  server.on("/beep",        hBeep);
  server.on("/play",        hPlay);
  server.on("/stopplay",    hStopPlay);
  server.on("/volume",      hVolume);
  server.on("/led/on",      hLedOn);
  server.on("/led/off",     hLedOff);
  server.on("/led/toggle",  hLedToggle);
  server.on("/sethz",       hSetHz);
  server.on("/trackcount",  hTrackCount);
  server.on("/rescan",      hRescan);
  server.on("/cleardial",   hClearDial);

  server.begin();
  Serial.println(F("[HTTP] Server started"));
}

// ===== Loop =====
void loop() {
  server.handleClient();
  ringLogic();
  ipAnnounceLogic(); // non-blocking LED IP blink

  // Rotary dial decoding
  if (dialTick) { dialTick = false; lastPulseSeenMs = millis(); }
  if (dialPulseCount > 0 && (millis() - lastPulseSeenMs) > DIAL_INTERDIGIT_MS) {
    uint8_t d = dialPulseCount % 10;           // 10 pulses -> 0
    uint8_t userDigit = (d == 0) ? 0 : d;      // user-facing 0..9
    lastDigit = userDigit;
    dialBuffer += String(userDigit);
    Serial.print(F("[DIAL] digit=")); Serial.print(userDigit);
    Serial.print(F(" (pulses=")); Serial.print(dialPulseCount); Serial.println(")");

    if (offHook) {
      // Default dial behavior: 0 -> track 10, 1..9 -> those tracks
      int t = (userDigit == 0) ? 10 : userDigit;
      dfSetVolume(currentVol);
      dfp.play(t);
      defaultLoop = false;
      queuedTrack = t;
    }
    dialPulseCount = 0;
  }

  // Hook detect
  bool nowOffHook = (digitalRead(PIN_HOOK) == HIGH);
  static bool lastOffHook = false;
  if (nowOffHook != lastOffHook) {
    lastOffHook = nowOffHook;
    offHook = nowOffHook;
    if (offHook) {
      ringing = false;
#if (RING_MODE == RING_MODE_PIEZO)
      piezoOff();
#else
      digitalWrite(PIN_RING, LOW);
#endif
      if (defaultLoop) {
        queuedTrack = 1;
        dfSetVolume(12);
        dfp.loop(1); // Dial (loop)
        Serial.println(F("[LOOP] Default track 1 looping @ vol 12"));
      } else {
        if (queuedTrack >= 1 && queuedTrack <= 5) playTrackSmart(queuedTrack);
        else { dfSetVolume(currentVol); dfp.play(queuedTrack); }
      }
    } else {
      dfp.stop();
      defaultLoop = true; // reset default mode
    }
  }
}
