// ============================================================
//  ESP8266 PAID WIFI  (voucher code + time limit + captive portal)
// ============================================================
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <LittleFS.h>
#include <LwipDhcpServer.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/napt.h>
extern "C" {
#include "user_interface.h"
}

// ------------------- CONFIG (eta change koro) ---------------
#define UP_SSID     "YOUR_ROUTER_WIFI"      // jei WiFi theke internet ashbe
#define UP_PASS     "YOUR_ROUTER_PASSWORD"
#define AP_SSID     "Paid WiFi"             // customer ra ei name dekhbe
#define AP_PASS     ""                      // khali = open WiFi (captive portal er jonno bhalo)
#define ADMIN_USER  "admin"
#define ADMIN_PASS  "admin123"
#define BKASH_NUMBER "01819193776"          // customer ei number e Send Money korbe
#define MAX_CLIENTS 6                       // ESP8266 e 5-6 er beshi na
#define COUNT_ONLY_WHEN_CONNECTED 1         // 1 = device disconnect thakle time kombe na
// ------------------------------------------------------------

// Package list: {name, minutes, price(Tk)}  - eta change korte paro
struct Pkg { const char* name; uint16_t minutes; uint16_t price; };
static const Pkg PKG[] = {
  {"1 Hour",   60,    10},
  {"3 Hours",  180,   25},
  {"1 Day",    1440,  50},
  {"7 Days",   10080, 250},
};
#define NPKG (sizeof(PKG) / sizeof(PKG[0]))
// ------------------------------------------------------------

#define NAPT      500
#define NAPT_PORT 10
#define MAX_V     100
#define MAX_S     12

static const IPAddress apIP(10, 10, 10, 1);
static const uint8_t   apB[4] = {10, 10, 10, 1};

struct Voucher { char code[9]; uint16_t minutes; bool used; };
struct Sess    { uint8_t mac[6]; uint32_t left; };

static Voucher V[MAX_V]; static int nV = 0;
static Sess    S[MAX_S];

// bKash payment request: status 0 = pending, 1 = approved, 2 = rejected
struct Req { char trx[16]; char phone[16]; uint8_t pkg; uint8_t mac[6]; uint8_t status; };
#define MAX_R 30
static Req R[MAX_R]; static int nR = 0;

ESP8266WebServer server(80);
WiFiUDP dnsIn, dnsUp;

static uint8_t  fails = 0;
static uint32_t lockUntil = 0;
static bool     sDirty = false;

// ===================== helpers ==============================
static String macStr(const uint8_t* m) {
  char b[18];
  sprintf(b, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(b);
}
static String macHex(const uint8_t* m) {
  char b[13];
  sprintf(b, "%02X%02X%02X%02X%02X%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(b);
}
static bool parseMacHex(const String& s, uint8_t* m) {
  if (s.length() != 12) return false;
  for (int i = 0; i < 6; i++) {
    String h = s.substring(i * 2, i * 2 + 2);
    m[i] = (uint8_t)strtoul(h.c_str(), nullptr, 16);
  }
  return true;
}
static String fmt(uint32_t s) {
  char b[20];
  sprintf(b, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60), (unsigned long)(s % 60));
  return String(b);
}

static bool isAuthorized(const uint8_t* mac) {
  for (int i = 0; i < MAX_S; i++)
    if (S[i].left > 0 && memcmp(S[i].mac, mac, 6) == 0) return true;
  return false;
}
static bool ipToMac(IPAddress ip, uint8_t* mac) {
  bool found = false;
  struct station_info* si = wifi_softap_get_station_info();
  for (struct station_info* s = si; s; s = STAILQ_NEXT(s, next)) {
    if (s->ip.addr == (uint32_t)ip) { memcpy(mac, s->bssid, 6); found = true; break; }
  }
  wifi_softap_free_station_info();
  return found;
}
static bool isAuthorizedIP(IPAddress ip) {
  uint8_t mac[6];
  return ipToMac(ip, mac) && isAuthorized(mac);
}

// ===================== storage ==============================
static void saveV() {
  File f = LittleFS.open("/v.txt", "w");
  if (!f) return;
  for (int i = 0; i < nV; i++) f.printf("%s,%u,%d\n", V[i].code, V[i].minutes, V[i].used ? 1 : 0);
  f.close();
}
static void loadV() {
  nV = 0;
  File f = LittleFS.open("/v.txt", "r");
  if (!f) return;
  while (f.available() && nV < MAX_V) {
    String l = f.readStringUntil('\n'); l.trim();
    int a = l.indexOf(','), b = l.lastIndexOf(',');
    if (a < 8 || b <= a) continue;
    l.substring(0, a).toCharArray(V[nV].code, 9);
    V[nV].minutes = l.substring(a + 1, b).toInt();
    V[nV].used = l.substring(b + 1).toInt() == 1;
    nV++;
  }
  f.close();
}
static void saveS() {
  File f = LittleFS.open("/s.txt", "w");
  if (!f) return;
  for (int i = 0; i < MAX_S; i++)
    if (S[i].left > 0) f.printf("%s,%lu\n", macHex(S[i].mac).c_str(), (unsigned long)S[i].left);
  f.close();
  sDirty = false;
}
static void loadS() {
  memset(S, 0, sizeof(S));
  File f = LittleFS.open("/s.txt", "r");
  if (!f) return;
  int i = 0;
  while (f.available() && i < MAX_S) {
    String l = f.readStringUntil('\n'); l.trim();
    int a = l.indexOf(',');
    if (a != 12) continue;
    if (!parseMacHex(l.substring(0, 12), S[i].mac)) continue;
    S[i].left = l.substring(a + 1).toInt();
    if (S[i].left > 0) i++;
  }
  f.close();
}

static void saveR() {
  File f = LittleFS.open("/r.txt", "w");
  if (!f) return;
  for (int i = 0; i < nR; i++)
    f.printf("%s,%s,%u,%s,%u\n", R[i].trx, R[i].phone, R[i].pkg, macHex(R[i].mac).c_str(), R[i].status);
  f.close();
}
static void loadR() {
  nR = 0;
  File f = LittleFS.open("/r.txt", "r");
  if (!f) return;
  while (f.available() && nR < MAX_R) {
    String l = f.readStringUntil('\n'); l.trim();
    int a = l.indexOf(',');
    int b = l.indexOf(',', a + 1);
    int c = l.indexOf(',', b + 1);
    int d = l.indexOf(',', c + 1);
    if (a < 1 || b < 0 || c < 0 || d < 0) continue;
    l.substring(0, a).toCharArray(R[nR].trx, 16);
    l.substring(a + 1, b).toCharArray(R[nR].phone, 16);
    int pk = l.substring(b + 1, c).toInt();
    R[nR].pkg = (pk >= 0 && pk < (int)NPKG) ? pk : 0;
    if (!parseMacHex(l.substring(c + 1, d), R[nR].mac)) continue;
    R[nR].status = l.substring(d + 1).toInt();
    nR++;
  }
  f.close();
}

// ===================== packet filter ========================
// Unauthorized device (code dey nai) shudhu DHCP, DNS(to ESP), HTTP(to ESP) pabe.
// Baki sob packet drop hobe, tai internet block thake.
static netif_input_fn origInput = nullptr;

static bool unauthAllowed(struct pbuf* p) {
  if (p->len < 14 + 20 + 4) return false;
  const uint8_t* b = (const uint8_t*)p->payload;
  uint8_t ihl = (b[14] & 0x0F) * 4;
  if (ihl < 20 || p->len < (uint16_t)(14 + ihl + 4)) return false;
  uint8_t proto = b[14 + 9];
  const uint8_t* dst = b + 14 + 16;
  const uint8_t* l4 = b + 14 + ihl;
  uint16_t dport = (l4[2] << 8) | l4[3];
  bool toEsp = memcmp(dst, apB, 4) == 0;
  if (proto == 17) {                       // UDP
    if (dport == 67) return true;          // DHCP
    if (dport == 53 && toEsp) return true; // DNS to ESP
    return false;
  }
  if (proto == 6) return toEsp && dport == 80;  // HTTP to ESP
  return false;
}

static err_t filterInput(struct pbuf* p, struct netif* inp) {
  if (p->len >= 14) {
    const uint8_t* b = (const uint8_t*)p->payload;
    if (b[12] == 0x08 && b[13] == 0x00) {          // IPv4
      if (!isAuthorized(b + 6) && !unauthAllowed(p)) {
        pbuf_free(p);
        return ERR_OK;
      }
    }
  }
  return origInput(p, inp);
}

static void installFilter() {
  for (struct netif* n = netif_list; n; n = n->next) {
    if (netif_ip4_addr(n)->addr == (uint32_t)apIP) {
      origInput = n->input;
      n->input = filterInput;
      Serial.println("Packet filter: OK");
      return;
    }
  }
  Serial.println("Packet filter: FAILED (netif not found)");
}

// ===================== DNS gate =============================
// Unauthorized -> sob domain er answer = ESP IP (captive portal)
// Authorized   -> real DNS te forward
static void dnsSpoof(uint8_t* q, int n, IPAddress cip, uint16_t cport) {
  int i = 12;
  while (i < n && q[i] != 0) i += q[i] + 1;
  i += 5;
  if (i > n) return;
  uint16_t qtype = (q[i - 4] << 8) | q[i - 3];
  uint8_t out[300];
  if (i > 270) return;
  memcpy(out, q, i);
  out[2] = 0x81; out[3] = 0x80;
  out[4] = 0; out[5] = 1;
  out[6] = 0; out[7] = 0; out[8] = 0; out[9] = 0; out[10] = 0; out[11] = 0;
  int len = i;
  if (qtype == 1) {  // A record
    out[7] = 1;
    const uint8_t ans[12] = {0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x04};
    memcpy(out + len, ans, 12); len += 12;
    memcpy(out + len, apB, 4);  len += 4;
  }
  dnsIn.beginPacket(cip, cport);
  dnsIn.write(out, len);
  dnsIn.endPacket();
}

static void dnsForward(uint8_t* q, int n, IPAddress cip, uint16_t cport) {
  IPAddress up = WiFi.dnsIP(0);
  if ((uint32_t)up == 0) up = IPAddress(8, 8, 8, 8);
  while (dnsUp.parsePacket() > 0) dnsUp.flush();
  dnsUp.beginPacket(up, 53);
  dnsUp.write(q, n);
  dnsUp.endPacket();
  uint32_t t = millis();
  while (millis() - t < 1500) {
    int m = dnsUp.parsePacket();
    if (m > 0 && m <= 512) {
      uint8_t buf[512];
      dnsUp.read(buf, m);
      if (m > 2 && buf[0] == q[0] && buf[1] == q[1]) {
        dnsIn.beginPacket(cip, cport);
        dnsIn.write(buf, m);
        dnsIn.endPacket();
        return;
      }
    }
    delay(1);
  }
}

static void handleDns() {
  int n = dnsIn.parsePacket();
  if (n <= 12 || n > 512) { if (n > 0) dnsIn.flush(); return; }
  uint8_t buf[512];
  dnsIn.read(buf, n);
  IPAddress cip = dnsIn.remoteIP();
  uint16_t cport = dnsIn.remotePort();
  if (isAuthorizedIP(cip)) dnsForward(buf, n, cip, cport);
  else dnsSpoof(buf, n, cip, cport);
}

// ===================== grant time ===========================
static bool grantTime(const uint8_t* mac, uint32_t minutes) {
  int si = -1;
  for (int i = 0; i < MAX_S; i++) if (S[i].left > 0 && memcmp(S[i].mac, mac, 6) == 0) { si = i; break; }
  if (si < 0) for (int i = 0; i < MAX_S; i++) if (S[i].left == 0) { si = i; break; }
  if (si < 0) return false;
  if (S[si].left == 0) memcpy(S[si].mac, mac, 6);
  S[si].left += minutes * 60UL;
  saveS();
  return true;
}

static int latestReqFor(const uint8_t* mac) {
  for (int i = nR - 1; i >= 0; i--) if (memcmp(R[i].mac, mac, 6) == 0) return i;
  return -1;
}
static bool trxExists(const String& t) {
  for (int i = 0; i < nR; i++) if (t == R[i].trx) return true;
  return false;
}

// ===================== web pages ============================
static const char HEAD[] PROGMEM =
  "<!DOCTYPE html><html><head><meta charset=utf-8>"
  "<meta name=viewport content='width=device-width,initial-scale=1'><title>WiFi</title><style>"
  "*{box-sizing:border-box}"
  "body{margin:0;min-height:100vh;font-family:system-ui,-apple-system,Segoe UI,sans-serif;"
  "background:linear-gradient(160deg,#0f172a,#1e1b4b);color:#e2e8f0;padding:14px}"
  ".c{max-width:440px;margin:14px auto;background:rgba(30,41,59,.9);padding:20px;border-radius:22px;"
  "box-shadow:0 10px 40px rgba(0,0,0,.45)}"
  "h2{margin:0 0 4px;font-size:24px}h3{margin:20px 0 8px}"
  ".sub{color:#94a3b8;font-size:14px;margin:4px 0 12px}"
  ".box{background:#0f172a;border:1px solid #334155;border-radius:16px;padding:14px;margin:12px 0}"
  ".e{color:#fca5a5;border-color:#7f1d1d}"
  "input[type=text],input[type=tel],input[type=number],select{width:100%;padding:13px;margin:6px 0;"
  "border-radius:10px;border:1px solid #334155;background:#1e293b;color:#fff;font-size:16px}"
  "button{width:100%;padding:13px;margin:8px 0 2px;border:0;border-radius:10px;font-size:16px;font-weight:700;"
  "color:#fff;background:#22c55e}"
  "button.bk{background:#e2136e}"
  ".pk{display:flex;flex-wrap:wrap;gap:8px;margin-top:10px}"
  ".pk input{display:none}"
  ".pk label{flex:1 1 45%;text-align:center;padding:10px 6px;border-radius:12px;background:#1e293b;"
  "border:2px solid #334155;font-size:14px;cursor:pointer}"
  ".pk input:checked+label{border-color:#e2136e;background:#3b0a24}"
  ".num{font-size:24px;font-weight:800;letter-spacing:1px;text-align:center;color:#fff;background:#e2136e33;"
  "border:2px dashed #e2136e;border-radius:12px;padding:10px;margin:10px 0}"
  ".t{font-size:40px;text-align:center;margin:14px 0;font-weight:800;color:#4ade80}"
  ".sp{width:36px;height:36px;border:4px solid #334155;border-top-color:#e2136e;border-radius:50%;"
  "margin:16px auto;animation:r 1s linear infinite}@keyframes r{to{transform:rotate(360deg)}}"
  "table{width:100%;border-collapse:collapse;font-size:13px}"
  "td,th{padding:6px 4px;border-bottom:1px solid #334155;text-align:left;vertical-align:top}"
  "a{color:#38bdf8}"
  "a.ok,a.no{display:inline-block;padding:7px 10px;margin:2px;border-radius:8px;color:#fff;text-decoration:none;font-weight:700}"
  "a.ok{background:#22c55e}a.no{background:#ef4444}"
  "</style></head><body><div class=c>";

static void sendPage(const String& body) {
  String h = FPSTR(HEAD);
  h += body;
  h += F("</div></body></html>");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", h);
}

static void loginPage(const String& msg) {
  String b = F("<h2>&#128246; WiFi Access</h2><p class=sub>Buy internet with bKash, or use a redeem code.</p>");
  if (msg.length()) { b += F("<div class='box e'>"); b += msg; b += F("</div>"); }

  b += F("<form method=POST action=/buy><div class=box><b>1. Choose a package</b><div class=pk>");
  for (int i = 0; i < NPKG; i++) {
    b += F("<input type=radio name=pkg id=p"); b += String(i);
    b += F(" value="); b += String(i);
    if (i == 0) b += F(" checked");
    b += F("><label for=p"); b += String(i);
    b += F("><b>"); b += PKG[i].name; b += F("</b><br>"); b += String(PKG[i].price); b += F(" Tk</label>");
  }
  b += F("</div></div><div class=box><b>2. Send Money on bKash</b>"
         "<div class=num onclick=cp()>");
  b += BKASH_NUMBER;
  b += F("</div><p class=sub>Tap the number to copy. Send the exact package price (Send Money), then fill in below.</p>"
         "<input type=tel name=phone placeholder='Your bKash number (01XXXXXXXXX)' maxlength=11>"
         "<input type=text name=trx placeholder='Transaction ID (TrxID)' autocapitalize=characters maxlength=14>"
         "<button class=bk>Submit &amp; wait for approval</button></div></form>");

  b += F("<div class=box><b>Have a redeem code?</b>"
         "<form method=POST action=/login>"
         "<input type=text name=code placeholder='Redeem code' autocomplete=off autocapitalize=characters>"
         "<button>Connect</button></form></div>");

  b += F("<script>function cp(){var t=document.createElement('textarea');t.value='");
  b += BKASH_NUMBER;
  b += F("';document.body.appendChild(t);t.select();try{document.execCommand('copy')}catch(e){}"
         "document.body.removeChild(t);alert('Number copied')}</script>");
  sendPage(b);
}

static void pendingPage(int ri) {
  String b = F("<h2>&#9203; Waiting for approval</h2>"
               "<p class=sub>Your payment request was sent. Internet starts automatically when the shop approves it.</p>"
               "<div class=sp></div><div class=box>Package: <b>");
  b += PKG[R[ri].pkg].name;
  b += F("</b><br>Amount: <b>"); b += String(PKG[R[ri].pkg].price);
  b += F(" Tk</b><br>TrxID: <b>"); b += R[ri].trx;
  b += F("</b></div><script>setInterval(function(){var x=new XMLHttpRequest();x.open('GET','/check');"
         "x.onload=function(){if(x.responseText!='pending')location.reload()};x.send()},4000)</script>");
  sendPage(b);
}

static void statusPage(uint32_t left) {
  String b = F("<h2>&#9989; You are online</h2><div class=t id=t>");
  b += fmt(left);
  b += F("</div><p class=sub style='text-align:center'>Time left. Time only runs while your device is connected to this WiFi.</p><script>var s=");
  b += String(left);
  b += F(";function p(n){return n<10?'0'+n:n}setInterval(function(){if(s>0)s--;"
         "document.getElementById('t').textContent=Math.floor(s/3600)+':'+p(Math.floor(s/60)%60)+':'+p(s%60)},1000)"
         "</script>");
  sendPage(b);
}

static void handleRoot() {
  uint8_t mac[6];
  if (ipToMac(server.client().remoteIP(), mac)) {
    for (int i = 0; i < MAX_S; i++)
      if (S[i].left > 0 && memcmp(S[i].mac, mac, 6) == 0) { statusPage(S[i].left); return; }
    int ri = latestReqFor(mac);
    if (ri >= 0 && R[ri].status == 0) { pendingPage(ri); return; }
    if (ri >= 0 && R[ri].status == 2) { loginPage("Your last payment request was rejected. Check the TrxID/amount and try again."); return; }
  }
  loginPage("");
}

static void handleCheck() {
  uint8_t mac[6];
  String r = "none";
  if (ipToMac(server.client().remoteIP(), mac)) {
    if (isAuthorized(mac)) r = "ok";
    else {
      int ri = latestReqFor(mac);
      if (ri >= 0) r = R[ri].status == 0 ? "pending" : (R[ri].status == 2 ? "rejected" : "none");
    }
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", r);
}

static void handleNotFound() {
  if (server.hostHeader() != apIP.toString()) {   // captive portal probe -> redirect
    server.sendHeader("Location", "http://" + apIP.toString() + "/", true);
    server.send(302, "text/plain", "");
    return;
  }
  handleRoot();
}

static void redirectTo(const char* url) {
  server.sendHeader("Location", url, true);
  server.send(303, "text/plain", "");
}

// ---- redeem code login ----
static void handleLogin() {
  uint8_t mac[6];
  if (!ipToMac(server.client().remoteIP(), mac)) { loginPage("Device not found. Turn WiFi off/on and try again."); return; }
  if (millis() < lockUntil) { loginPage("Too many wrong codes. Try again in a while."); return; }

  String code = server.arg("code");
  code.trim(); code.toUpperCase();
  int vi = -1;
  for (int i = 0; i < nV; i++)
    if (!V[i].used && code == V[i].code) { vi = i; break; }

  if (vi < 0) {
    if (++fails >= 5) { lockUntil = millis() + 30000; fails = 0; }
    loginPage("Wrong or already used code.");
    return;
  }
  fails = 0;
  if (!grantTime(mac, V[vi].minutes)) { loginPage("Server busy. Try again later."); return; }
  V[vi].used = true;
  saveV();
  redirectTo("/");
}

// ---- bKash payment request ----
static bool allAlnum(const String& s) {
  for (unsigned i = 0; i < s.length(); i++) if (!isalnum((unsigned char)s[i])) return false;
  return true;
}
static bool allDigits(const String& s) {
  for (unsigned i = 0; i < s.length(); i++) if (!isdigit((unsigned char)s[i])) return false;
  return true;
}

static void handleBuy() {
  uint8_t mac[6];
  if (!ipToMac(server.client().remoteIP(), mac)) { loginPage("Device not found. Turn WiFi off/on and try again."); return; }
  int ri = latestReqFor(mac);
  if (ri >= 0 && R[ri].status == 0) { redirectTo("/"); return; }

  int pk = server.arg("pkg").toInt();
  if (pk < 0 || pk >= NPKG) pk = 0;
  String trx = server.arg("trx"); trx.trim(); trx.toUpperCase();
  String ph = server.arg("phone"); ph.trim();

  if (trx.length() < 6 || trx.length() > 14 || !allAlnum(trx)) { loginPage("Enter a valid TrxID (letters and numbers only)."); return; }
  if (ph.length() != 11 || !allDigits(ph) || !ph.startsWith("01")) { loginPage("Enter your 11 digit bKash number (01XXXXXXXXX)."); return; }
  if (trxExists(trx)) { loginPage("This TrxID is already submitted."); return; }

  if (nR >= MAX_R) {                       // purono finished request muche jayga banao
    int k = 0;
    for (int i = 0; i < nR; i++) if (R[i].status == 0 || i >= nR - 10) R[k++] = R[i];
    nR = k;
  }
  if (nR >= MAX_R) { loginPage("Server busy. Try again later."); return; }

  trx.toCharArray(R[nR].trx, 16);
  ph.toCharArray(R[nR].phone, 16);
  R[nR].pkg = pk;
  memcpy(R[nR].mac, mac, 6);
  R[nR].status = 0;
  nR++;
  saveR();
  redirectTo("/");
}

// ===================== admin ================================
static bool adminOk() {
  if (!server.authenticate(ADMIN_USER, ADMIN_PASS)) { server.requestAuthentication(); return false; }
  return true;
}
static void redirectAdmin() { redirectTo("/admin"); }

static void genCode(char* out) {
  const char* A = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  for (;;) {
    for (int i = 0; i < 8; i++) { out[i] = A[(RANDOM_REG32 ^ micros()) % 32]; delayMicroseconds(7); }
    out[8] = 0;
    bool dup = false;
    for (int i = 0; i < nV; i++) if (strcmp(V[i].code, out) == 0) dup = true;
    if (!dup) return;
  }
}

static void handleAdmin() {
  if (!adminOk()) return;
  String b = F("<h2>Admin Panel</h2><p class=sub>Upstream: ");
  if (WiFi.status() == WL_CONNECTED) { b += F("connected, "); b += WiFi.localIP().toString(); b += F(", "); b += String(WiFi.RSSI()); b += F(" dBm"); }
  else b += F("<span style='color:#f87171'>NOT connected</span>");
  b += F("<br>Devices on WiFi: "); b += String(WiFi.softAPgetStationNum());
  b += F(" &middot; Free heap: "); b += String(ESP.getFreeHeap()); b += F("</p>");

  // ---- pending bKash requests ----
  int pend = 0;
  String rows;
  for (int i = nR - 1; i >= 0; i--) {
    if (R[i].status != 0) continue;
    pend++;
    rows += F("<tr><td><b>"); rows += PKG[R[i].pkg].name; rows += F("</b><br>"); rows += String(PKG[R[i].pkg].price);
    rows += F(" Tk</td><td>"); rows += R[i].phone; rows += F("<br><b>"); rows += R[i].trx;
    rows += F("</b><br><small>"); rows += macStr(R[i].mac);
    rows += F("</small></td><td><a class=ok href='/admin/approve?i="); rows += String(i);
    rows += F("'>Approve</a><a class=no href='/admin/reject?i="); rows += String(i); rows += F("'>Reject</a></td></tr>");
  }
  b += F("<div class=box><b>bKash requests ("); b += String(pend); b += F(" pending)</b>");
  if (pend) { b += F("<table><tr><th>Package</th><th>Sender / TrxID</th><th></th></tr>"); b += rows; b += F("</table>"); }
  else b += F("<p class=sub>No pending requests.</p>");
  b += F("</div>");

  // ---- active devices ----
  b += F("<div class=box><b>Active devices</b><table><tr><th>MAC</th><th>Left</th><th></th></tr>");
  for (int i = 0; i < MAX_S; i++)
    if (S[i].left > 0) {
      b += F("<tr><td>"); b += macStr(S[i].mac); b += F("</td><td>"); b += fmt(S[i].left);
      b += F("</td><td><a href='/admin/kick?i="); b += String(i); b += F("'>remove</a></td></tr>");
    }
  b += F("</table></div>");

  // ---- redeem codes ----
  b += F("<div class=box><b>Make redeem codes</b><form method=POST action=/admin/gen>"
         "<input type=number name=n min=1 max=50 value=5 placeholder='How many'>"
         "<select name=m><option value=30>30 minutes</option><option value=60 selected>1 hour</option>"
         "<option value=180>3 hours</option><option value=720>12 hours</option>"
         "<option value=1440>1 day</option><option value=10080>7 days</option></select>"
         "<button>Generate</button></form>");
  int unused = 0;
  String vr;
  for (int i = 0; i < nV; i++)
    if (!V[i].used) { unused++; vr += F("<tr><td><b>"); vr += V[i].code; vr += F("</b></td><td>"); vr += String(V[i].minutes); vr += F(" min</td></tr>"); }
  b += F("<p class=sub>Unused codes: "); b += String(unused); b += F("</p>");
  if (unused) { b += F("<table><tr><th>Code</th><th>Time</th></tr>"); b += vr; b += F("</table>"); }
  b += F("<p><a href=/admin/clean>Delete used codes</a></p></div>");

  // ---- history ----
  b += F("<div class=box><b>Recent requests</b><table><tr><th>Package</th><th>Sender / TrxID</th><th></th></tr>");
  int shown = 0;
  for (int i = nR - 1; i >= 0 && shown < 10; i--) {
    if (R[i].status == 0) continue;
    shown++;
    b += F("<tr><td>"); b += PKG[R[i].pkg].name; b += F("</td><td>"); b += R[i].phone; b += F("<br>"); b += R[i].trx;
    b += F("</td><td>"); b += R[i].status == 1 ? F("approved") : F("rejected"); b += F("</td></tr>");
  }
  b += F("</table></div>");

  b += F("<script>setTimeout(function(){var a=document.activeElement;"
         "if(!a||(a.tagName!='INPUT'&&a.tagName!='SELECT'))location.reload()},10000)</script>");
  sendPage(b);
}

static void handleApprove() {
  if (!adminOk()) return;
  int i = server.arg("i").toInt();
  if (i >= 0 && i < nR && R[i].status == 0) {
    if (grantTime(R[i].mac, PKG[R[i].pkg].minutes)) { R[i].status = 1; saveR(); }
  }
  redirectAdmin();
}

static void handleReject() {
  if (!adminOk()) return;
  int i = server.arg("i").toInt();
  if (i >= 0 && i < nR && R[i].status == 0) { R[i].status = 2; saveR(); }
  redirectAdmin();
}

static void handleGen() {
  if (!adminOk()) return;
  int n = constrain(server.arg("n").toInt(), 1, 50);
  int m = constrain(server.arg("m").toInt(), 1, 43200);
  for (int i = 0; i < n && nV < MAX_V; i++) {
    genCode(V[nV].code);
    V[nV].minutes = m;
    V[nV].used = false;
    nV++;
  }
  saveV();
  redirectAdmin();
}

static void handleClean() {
  if (!adminOk()) return;
  int k = 0;
  for (int i = 0; i < nV; i++) if (!V[i].used) V[k++] = V[i];
  nV = k;
  saveV();
  redirectAdmin();
}

static void handleKick() {
  if (!adminOk()) return;
  int i = server.arg("i").toInt();
  if (i >= 0 && i < MAX_S) { S[i].left = 0; saveS(); }
  redirectAdmin();
}

// ===================== timer ================================
static void tick() {
  static uint32_t last = 0, lastSave = 0;
  if (millis() - last < 1000) return;
  last = millis();
  struct station_info* si = wifi_softap_get_station_info();
  for (int i = 0; i < MAX_S; i++) {
    if (S[i].left == 0) continue;
    bool present = !COUNT_ONLY_WHEN_CONNECTED;
    for (struct station_info* s = si; s && !present; s = STAILQ_NEXT(s, next))
      if (memcmp(s->bssid, S[i].mac, 6) == 0) present = true;
    if (present) {
      S[i].left--;
      sDirty = true;
      if (S[i].left == 0) lastSave = 0;   // expire hole sathe sathe save
    }
  }
  wifi_softap_free_station_info();
  if (sDirty && millis() - lastSave > 60000) { saveS(); lastSave = millis(); }
}

// ===================== setup / loop =========================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nPaid WiFi starting...");

  LittleFS.begin();
  loadV();
  loadS();
  loadR();

  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(UP_SSID, UP_PASS);
  Serial.print("Connecting to router");
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) { delay(500); Serial.print("."); }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) Serial.println("Router IP: " + WiFi.localIP().toString());
  else Serial.println("Router connect hoy nai (pore auto-retry korbe)");

  dhcpSoftAP.dhcps_set_dns(0, apIP);   // client der DNS = ESP (captive portal)
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, strlen(AP_PASS) ? AP_PASS : nullptr, WiFi.channel(), 0, MAX_CLIENTS);

  err_t ret = ip_napt_init(NAPT, NAPT_PORT);
  if (ret == ERR_OK) ret = ip_napt_enable_no(SOFTAP_IF, 1);
  Serial.printf("NAPT: %s\n", ret == ERR_OK ? "OK" : "FAILED");

  dnsIn.begin(53);
  dnsUp.begin(5300);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/login", HTTP_POST, handleLogin);
  server.on("/buy", HTTP_POST, handleBuy);
  server.on("/check", HTTP_GET, handleCheck);
  server.on("/admin", HTTP_GET, handleAdmin);
  server.on("/admin/gen", HTTP_POST, handleGen);
  server.on("/admin/clean", HTTP_GET, handleClean);
  server.on("/admin/kick", HTTP_GET, handleKick);
  server.on("/admin/approve", HTTP_GET, handleApprove);
  server.on("/admin/reject", HTTP_GET, handleReject);
  server.onNotFound(handleNotFound);
  server.begin();

  installFilter();
  Serial.println("Ready. Admin: http://10.10.10.1/admin");
}

void loop() {
  server.handleClient();
  handleDns();
  tick();
}
