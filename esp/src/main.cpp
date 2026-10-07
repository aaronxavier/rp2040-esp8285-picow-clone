// ESP8285 side: WiFi, web UI, telemetry. Talks to the Pico over UART0 with text lines:
//   ESP -> Pico:  "WIFI <0|1> <ip>"              every second
//                 "TEXT <rrggbb> <speed> <rainbow 0|1> <text>"
//                                                 scroll text, speed in columns/s (1-60)
//                 "IMG <384 hex>"                 8x8 image, rrggbb per pixel, row-major from top-left
//   Pico -> ESP:  "T <anything>"                  latest telemetry, served at /telemetry
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ArduinoOTA.h>
#include "secrets.h"

static ESP8266WebServer server(80);
static String telemetry, rx;

static const char PAGE[] PROGMEM = R"HTML(<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>LED Matrix</title>
<style>
body{font-family:system-ui,sans-serif;max-width:26rem;margin:1rem auto;padding:0 1rem;background:#111;color:#eee}
fieldset{border:1px solid #444;border-radius:8px;margin:0 0 1rem;padding:.75rem}
label{display:flex;gap:.5rem;align-items:center;margin:.5rem 0}
input[type=text]{width:100%;box-sizing:border-box;font-size:1.1rem;padding:.4rem}
input[type=range]{flex:1}
button{font-size:1rem;padding:.4rem 1rem}
canvas{width:128px;height:128px;image-rendering:pixelated;background:#000;display:block;margin:.5rem 0}
small{color:#888}
</style>
<h2>LED Matrix</h2>
<label>Brightness <input id=bright type=range min=5 max=100 value=40> <span id=brightv></span>%</label>
<fieldset><legend>Scrolling text</legend>
<input id=text type=text maxlength=200 value="Hello!">
<label>Color <input id=color type=color value="#00ff40">
<input id=rainbow type=checkbox> Surprise me</label>
<label>Speed <input id=speed type=range min=1 max=60 value=12> <span id=speedv></span> px/s</label>
<button id=showText>Show text</button></fieldset>
<fieldset><legend>Image</legend>
<input id=file type=file accept="image/*">
<canvas id=prev width=8 height=8></canvas>
<button id=showImg disabled>Show image</button></fieldset>
<small>Pico: <span id=tel>...</span></small>
<script>
const q = id => document.getElementById(id), hex = n => Math.round(n).toString(16).padStart(2, '0');
const k = () => q('bright').value / 100;
for (const id of ['bright', 'speed']) (q(id).oninput = () => q(id + 'v').textContent = q(id).value)();

q('showText').onclick = () => {
  const rb = q('rainbow').checked, c = rb ? '#ffffff' : q('color').value;  // rainbow: Pico uses only the brightness
  const rgb = [1, 3, 5].map(i => hex(parseInt(c.substr(i, 2), 16) * k())).join('');
  fetch('/text?' + new URLSearchParams({s: q('text').value, c: rgb, v: q('speed').value, r: +rb}));
};
q('rainbow').onchange = () => q('color').disabled = q('rainbow').checked;
q('text').onkeydown = e => { if (e.key == 'Enter') q('showText').click(); };  // must not return false: that cancels typing

// Center-crop to a square, then halve repeatedly so the 8x8 result averages all pixels.
q('file').onchange = async () => {
  const img = await createImageBitmap(q('file').files[0]);
  let s = Math.min(img.width, img.height), cv = document.createElement('canvas');
  cv.width = cv.height = s;
  cv.getContext('2d').drawImage(img, (img.width - s) / 2, (img.height - s) / 2, s, s, 0, 0, s, s);
  while (s > 8) {
    const n = Math.max(8, s >> 1), t = document.createElement('canvas');
    t.width = t.height = n;
    t.getContext('2d').drawImage(cv, 0, 0, n, n);
    cv = t; s = n;
  }
  const p = q('prev').getContext('2d');
  p.clearRect(0, 0, 8, 8);
  p.drawImage(cv, 0, 0);
  q('showImg').disabled = false;
};
q('showImg').onclick = () => {
  const d = q('prev').getContext('2d').getImageData(0, 0, 8, 8).data;
  let h = '';
  for (let i = 0; i < d.length; i += 4)
    for (let j = 0; j < 3; j++) h += hex(255 * (d[i + j] * d[i + 3] / 65025) ** 2.2 * k());  // gamma: LEDs are linear
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
    if (!isHex(d, 384)) return server.send(400, "text/plain", "need d = 64 x rrggbb");
    Serial.printf("IMG %s\n", d.c_str());
    server.send(204);
}

void setup() {
    Serial.begin(115200);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    server.on("/", [] { server.send_P(200, "text/html", PAGE); });
    server.on("/text", handleText);
    server.on("/img", handleImg);
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

    static uint32_t last;
    if (millis() - last >= 1000) {  // resend so a rebooted Pico picks it up
        last = millis();
        bool up = WiFi.status() == WL_CONNECTED;
        Serial.printf("WIFI %d %s\n", up, WiFi.localIP().toString().c_str());
    }
}
