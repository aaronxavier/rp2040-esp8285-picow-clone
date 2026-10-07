// ESP8285 side: WiFi, web UI, telemetry. Talks to the Pico over UART0 with text lines:
//   ESP -> Pico:  "WIFI <0|1> <ip>"              every second
//                 "TEXT <rrggbb> <speed> <rainbow 0|1> <text>"
//                                                 scroll text, speed in columns/s (1-60)
//                 "IMG <768 hex>"                 16x8 frame, rrggbb per pixel, row-major from top-left
//                 "TIME <secs>"                   local seconds since midnight, sent on each new second (NTP)
//                 "CLOCK <rrggbb>"                show the clock
//                 "BRIGHT <percent>"              global brightness, sent on change and every second
//   Colors are perceptual (as in a color picker); the Pico applies gamma and brightness.
//                 "TIMER <secs> <rrggbb>"         countdown, flashes at 0 then back to the clock; 0 cancels
//   Pico -> ESP:  "T <anything>"                  latest telemetry, served at /telemetry
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ArduinoOTA.h>
#include <time.h>
#include "secrets.h"

#define TZ_INFO "CET-1CEST,M3.5.0,M10.5.0/3"  // POSIX TZ for Europe/Berlin; change for your time zone

static ESP8266WebServer server(80);
static String telemetry, rx;
static int brightPct = 5;

static const char PAGE[] PROGMEM = R"HTML(<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>LED Matrix</title>
<style>
body{font-family:system-ui,sans-serif;max-width:26rem;margin:1rem auto;padding:0 1rem;background:#111;color:#eee}
fieldset{border:1px solid #444;border-radius:8px;margin:0 0 1rem;padding:.75rem}
label{display:flex;gap:.5rem;align-items:center;margin:.5rem 0}
input[type=text]{width:100%;box-sizing:border-box;font-size:1.1rem;padding:.4rem}
input[type=range]{flex:1}
button{font-size:1rem;padding:.4rem 1rem}
canvas{width:256px;height:128px;image-rendering:pixelated;background:#000;display:block;margin:.5rem 0}
small{color:#888}
</style>
<h2>LED Matrix</h2>
<label>Brightness <input id=bright type=range min=1 max=100 value=5> <span id=brightv></span>%</label>
<fieldset><legend>Scrolling text</legend>
<input id=text type=text maxlength=200 value="Hello!">
<label>Color <input id=color type=color value="#00ff40">
<input id=rainbow type=checkbox> Surprise me</label>
<label>Speed <input id=speed type=range min=1 max=60 value=12> <span id=speedv></span> px/s</label>
<button id=showText>Show text</button></fieldset>
<fieldset><legend>Clock &amp; timer</legend>
<label>Color <input id=ccolor type=color value="#ff8000"> <button id=showClock>Show clock</button></label>
<label>Timer <input id=tmin type=number min=0 max=1440 value=5 style="width:4.5em"> min
<input id=tsec type=number min=0 max=59 value=0 style="width:3.5em"> s</label>
<button id=startTimer>Start timer</button> <button id=cancelTimer>Cancel</button>
<p>Quick: <button class=quick data-m=1>1 min</button> <button class=quick data-m=5>5 min</button>
<button class=quick data-m=10>10 min</button> <button class=quick data-m=25>25 min</button></fieldset>
<fieldset><legend>Image</legend>
<input id=file type=file accept="image/*">
<canvas id=prev width=16 height=8></canvas>
<button id=showImg disabled>Show image</button></fieldset>
<small>Pico: <span id=tel>...</span></small>
<script>
const W = 16, H = 8;  // display size: two 8x8 panels side by side
const q = id => document.getElementById(id), hex = n => Math.round(n).toString(16).padStart(2, '0');
const rgb = c => c.slice(1);  // '#rrggbb' -> 'rrggbb'
fetch('/bright').then(r => r.text()).then(b => { q('bright').value = b; q('bright').oninput(); });
q('bright').onchange = () => fetch('/bright?b=' + q('bright').value);
for (const id of ['bright', 'speed']) (q(id).oninput = () => q(id + 'v').textContent = q(id).value)();

q('showText').onclick = () => {
  const rb = q('rainbow').checked, c = rb ? '#ffffff' : q('color').value;  // rainbow: Pico ignores the color
  fetch('/text?' + new URLSearchParams({s: q('text').value, c: rgb(c), v: q('speed').value, r: +rb}));
};
q('showClock').onclick = () => fetch('/clock?c=' + rgb(q('ccolor').value));
const timer = s => fetch('/timer?' + new URLSearchParams({s, c: rgb(q('ccolor').value)}));
q('startTimer').onclick = () => timer(q('tmin').value * 60 + +q('tsec').value);
q('cancelTimer').onclick = () => timer(0);
for (const b of document.querySelectorAll('.quick')) b.onclick = () => timer(b.dataset.m * 60);
q('rainbow').onchange = () => q('color').disabled = q('rainbow').checked;
q('text').onkeydown = e => { if (e.key == 'Enter') q('showText').click(); };  // must not return false: that cancels typing

// Center-crop to the display's aspect, then halve repeatedly so the result averages all pixels.
q('file').onchange = async () => {
  const img = await createImageBitmap(q('file').files[0]);
  let h = Math.floor(Math.min(img.height, img.width * H / W)), w = h * W / H, cv = document.createElement('canvas');
  cv.width = w; cv.height = h;
  cv.getContext('2d').drawImage(img, (img.width - w) / 2, (img.height - h) / 2, w, h, 0, 0, w, h);
  while (h > H) {
    const nh = Math.max(H, h >> 1), nw = nh * W / H, t = document.createElement('canvas');
    t.width = nw; t.height = nh;
    t.getContext('2d').drawImage(cv, 0, 0, nw, nh);
    cv = t; h = nh;
  }
  const p = q('prev').getContext('2d');
  p.clearRect(0, 0, W, H);
  p.drawImage(cv, 0, 0, W, H);
  q('showImg').disabled = false;
};
q('showImg').onclick = () => {
  const d = q('prev').getContext('2d').getImageData(0, 0, W, H).data;
  let h = '';
  for (let i = 0; i < d.length; i += 4)
    for (let j = 0; j < 3; j++) h += hex(d[i + j] * d[i + 3] / 255);  // transparent -> dark; Pico does gamma
  fetch('/img?d=' + h);
};
setInterval(async () => { try { q('tel').textContent = await (await fetch('/telemetry')).text(); } catch (e) {} }, 2000);
</script>)HTML";

static bool isHex(const String &s, unsigned len) {
    if (s.length() != len) return false;
    for (char ch : s) if (!isxdigit(ch)) return false;
    return true;
}

static void handleText() {
    String c = server.arg("c"), in = server.arg("s"), s;
    long v = server.arg("v").toInt();
    if (!isHex(c, 6) || v < 1 || v > 60) return server.send(400, "text/plain", "need c=rrggbb, v=1..60");
    for (unsigned char ch : in) {  // printable ASCII only; also keeps \n out of the protocol
        if ((ch & 0xC0) == 0x80) continue;  // UTF-8 continuation byte
        s += ch >= 32 && ch < 127 ? (char)ch : '?';
        if (s.length() >= 200) break;
    }
    Serial.printf("TEXT %s %ld %d %s\n", c.c_str(), v, server.arg("r") == "1", s.c_str());
    server.send(204);
}

static void handleImg() {
    String d = server.arg("d");
    if (!isHex(d, 16 * 8 * 6)) return server.send(400, "text/plain", "need d = 128 x rrggbb (16x8)");
    Serial.printf("IMG %s\n", d.c_str());
    server.send(204);
}

static void handleBright() {
    if (server.hasArg("b")) {
        long b = server.arg("b").toInt();
        if (b < 1 || b > 100) return server.send(400, "text/plain", "need b=1..100 (percent)");
        brightPct = b;
        Serial.printf("BRIGHT %d\n", brightPct);
    }
    server.send(200, "text/plain", String(brightPct));
}

static void handleClock() {
    String c = server.arg("c");
    if (!isHex(c, 6)) return server.send(400, "text/plain", "need c=rrggbb");
    Serial.printf("CLOCK %s\n", c.c_str());
    server.send(204);
}

static void handleTimer() {
    String c = server.arg("c");
    long s = server.arg("s").toInt();
    if (!isHex(c, 6) || s < 0 || s > 86400) return server.send(400, "text/plain", "need c=rrggbb, s=0..86400 (0 cancels)");
    Serial.printf("TIMER %ld %s\n", s, c.c_str());
    server.send(204);
}

void setup() {
    Serial.begin(115200);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    configTime(TZ_INFO, "pool.ntp.org", "time.google.com");
    server.on("/", [] { server.send_P(200, "text/html", PAGE); });
    server.on("/text", handleText);
    server.on("/img", handleImg);
    server.on("/clock", handleClock);
    server.on("/bright", handleBright);
    server.on("/timer", handleTimer);
    server.on("/telemetry", [] { server.send(200, "text/plain", telemetry); });
    server.begin();
    ArduinoOTA.setHostname("pico-esp");  // after first serial flash: pio run -e ota -t upload
    ArduinoOTA.begin();
}

void loop() {
    server.handleClient();
    ArduinoOTA.handle();

    while (Serial.available()) {
        char ch = Serial.read();
        if (ch != '\n') { if (rx.length() < 200) rx += ch; continue; }
        if (rx.startsWith("T ")) telemetry = rx.substring(2);
        rx = "";
    }

    static time_t last_t;
    time_t t = time(nullptr);
    if (t > 1700000000 && t != last_t) {  // NTP synced, new second
        last_t = t;
        struct tm lt;
        localtime_r(&t, &lt);
        Serial.printf("TIME %d\n", lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec);
    }

    static uint32_t last;
    if (millis() - last >= 1000) {  // resend so a rebooted Pico picks it up
        last = millis();
        bool up = WiFi.status() == WL_CONNECTED;
        Serial.printf("WIFI %d %s\n", up, WiFi.localIP().toString().c_str());
        Serial.printf("BRIGHT %d\n", brightPct);
    }
}
