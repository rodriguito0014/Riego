#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <esp_sntp.h>
#include "secrets.h"  // WIFI_SSID, WIFI_PASS, MQTT_HOST, MQTT_USER, MQTT_PASS (copia secrets.example.h)

const int   MQTT_PORT = 8883;
const char* BASE      = "riego/casa";
const char* TZ_INFO   = "CET-1CEST,M3.5.0,M10.5.0/3";
const int   NZ = 6;
const int   RELAY_PINS[NZ] = {32, 33, 25, 26, 27, 4};
const bool  RELAY_ACTIVE_LOW = true;
const unsigned long CLOCK_EVERY = 10000;

const int MAX_STARTS = 8;
const int MAX_LOG = 20;
WiFiClientSecure net;
PubSubClient mqtt(net);
Preferences prefs;

int    zoneMin[NZ];
int    starts[MAX_STARTS];
int    nStarts = 0;
bool   days[7];
String savedAt = "";

bool running = false;
int  runList[NZ], runDur[NZ], runLen = 0, runIdx = 0, pending = 0;
uint64_t zoneStartMs = 0;
int  lastMinute = -1;
bool timeOk = false;

String evLog[MAX_LOG];
int    evCount = 0;

bool dirty = true, logDirty = true;
unsigned long lastTry = 0, lastClockPub = 0;
const char* DN[] = {"L", "M", "X", "J", "V", "S", "D"};

uint64_t clk() { return millis(); }
int speed() { return 1; }
void tickClock() {}
bool timeValid(struct tm& t) { return getLocalTime(&t, 10) && t.tm_year > 120; }

String two(int n) { return (n < 10 ? "0" : "") + String(n); }
String hhmm(int m) { return two(m / 60) + ":" + two(m % 60); }
String topic(const char* t) { return String(BASE) + "/" + t; }

void addEv(const String& msg) {
  struct tm t;
  String stamp = timeValid(t) ? String(DN[(t.tm_wday + 6) % 7]) + " " + two(t.tm_hour) + ":" + two(t.tm_min) : "--:--";
  for (int i = min(evCount, MAX_LOG - 1); i > 0; i--) evLog[i] = evLog[i - 1];
  evLog[0] = stamp + "|" + msg;
  if (evCount < MAX_LOG) evCount++;
  logDirty = true;
  Serial.println(stamp + "  " + msg);
}

void setRelay(int z, bool on) { digitalWrite(RELAY_PINS[z], (on ^ RELAY_ACTIVE_LOW) ? HIGH : LOW); }
void allOff() { for (int i = 0; i < NZ; i++) setRelay(i, false); }

void startZone(int k) {
  allOff();
  runIdx = k;
  zoneStartMs = clk();
  setRelay(runList[k], true);
  addEv("Riega zona " + String(runList[k] + 1) + " durante " + String(runDur[k]) + " min");
}

void startCycle(const String& src) {
  int n = 0;
  int list[NZ], dur[NZ];
  for (int i = 0; i < NZ; i++) if (zoneMin[i] > 0) { list[n] = i; dur[n] = zoneMin[i]; n++; }
  if (n == 0) { addEv("Ciclo " + src + ": ninguna zona tiene minutos"); return; }
  if (running) { pending++; addEv("Ciclo " + src + " en espera: hay otro en curso"); return; }
  for (int i = 0; i < n; i++) { runList[i] = list[i]; runDur[i] = dur[i]; }
  runLen = n;
  running = true;
  addEv("Inicio ciclo " + src);
  startZone(0);
}

void nextZone() {
  setRelay(runList[runIdx], false);
  if (runIdx + 1 >= runLen) {
    running = false;
    allOff();
    addEv("Ciclo terminado");
    if (pending > 0) { pending--; startCycle("en espera"); }
  } else {
    startZone(runIdx + 1);
  }
}

void stopCycle() {
  if (!running) { addEv("Parar: no hay ningún ciclo en curso"); return; }
  allOff();
  running = false;
  pending = 0;
  addEv("Ciclo parado desde la web");
  lastClockPub = 0;  // envía el reloj enseguida para que la web lo vea
}

void parseStarts(String s) {
  nStarts = 0;
  s += ",";
  int p = 0, c;
  while ((c = s.indexOf(',', p)) >= 0 && nStarts < MAX_STARTS) {
    String tok = s.substring(p, c);
    tok.trim();
    p = c + 1;
    if (tok.length() != 5 || tok[2] != ':') continue;
    int h = tok.substring(0, 2).toInt(), m = tok.substring(3).toInt();
    if (h < 0 || h > 23 || m < 0 || m > 59) continue;
    int v = h * 60 + m;
    bool dup = false;
    for (int i = 0; i < nStarts; i++) if (starts[i] == v) dup = true;
    if (!dup) starts[nStarts++] = v;
  }
  for (int i = 0; i < nStarts; i++)
    for (int j = i + 1; j < nStarts; j++)
      if (starts[j] < starts[i]) { int t = starts[i]; starts[i] = starts[j]; starts[j] = t; }
}

String startsStr() { String o; for (int i = 0; i < nStarts; i++) { if (i) o += ","; o += hhmm(starts[i]); } return o; }
String daysStr() { String o; for (int i = 0; i < 7; i++) o += days[i] ? '1' : '0'; return o; }
String minsStr() { String o; for (int i = 0; i < NZ; i++) { if (i) o += ","; o += String(zoneMin[i]); } return o; }
String daysNice() { String o; for (int i = 0; i < 7; i++) if (days[i]) o += DN[i]; return o.length() ? o : "ninguno"; }

void saveCfg() {
  for (int i = 0; i < NZ; i++) prefs.putInt(("m" + String(i)).c_str(), zoneMin[i]);
  prefs.putString("starts", startsStr());
  prefs.putString("days", daysStr());
  prefs.putString("saved", savedAt);
}

void loadCfg() {
  const int defMin[NZ] = {15, 10, 20, 10, 5, 15};
  for (int i = 0; i < NZ; i++) zoneMin[i] = prefs.getInt(("m" + String(i)).c_str(), defMin[i]);
  parseStarts(prefs.getString("starts", "07:00"));
  String d = prefs.getString("days", "1111111");
  for (int i = 0; i < 7; i++) days[i] = (i < (int)d.length() && d[i] == '1');
  savedAt = prefs.getString("saved", "");
}

void publishState() {
  JsonDocument d;
  JsonArray dy = d["days"].to<JsonArray>();
  for (int i = 0; i < 7; i++) dy.add(days[i]);
  JsonArray s = d["starts"].to<JsonArray>();
  for (int i = 0; i < nStarts; i++) s.add(hhmm(starts[i]));
  JsonArray z = d["zones"].to<JsonArray>();
  for (int i = 0; i < NZ; i++) z.add(zoneMin[i]);
  d["saved"] = savedAt;
  String out;
  serializeJson(d, out);
  if (mqtt.publish(topic("estado").c_str(), (const uint8_t*)out.c_str(), out.length(), true)) dirty = false;
}

void publishLog() {
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (int i = 0; i < evCount; i++) a.add(evLog[i]);
  String out;
  serializeJson(d, out);
  if (mqtt.publish(topic("log").c_str(), (const uint8_t*)out.c_str(), out.length(), true)) logDirty = false;
}

void publishClock() {
  struct tm t;
  if (!timeValid(t)) return;
  JsonDocument d;
  d["d"] = DN[(t.tm_wday + 6) % 7];
  d["t"] = two(t.tm_hour) + ":" + two(t.tm_min);
  d["x"] = speed();
  if (running) {
    d["z"] = runList[runIdx] + 1;
    int64_t left = (int64_t)runDur[runIdx] * 60 - (int64_t)((clk() - zoneStartMs) / 1000);
    d["left"] = left < 0 ? 0 : (long)left;
  }
  String out;
  serializeJson(d, out);
  mqtt.publish(topic("reloj").c_str(), out.c_str());
}

void onMessage(char* tp, byte* payload, unsigned int len) {
  String tps(tp);
  String p;
  p.reserve(len);
  for (unsigned int i = 0; i < len; i++) p += (char)payload[i];
  if (tps == topic("cmd")) { if (p == "stop") stopCycle(); return; }
  if (tps != topic("config")) return;
  JsonDocument d;
  if (deserializeJson(d, p)) { addEv("Programa recibido con errores, ignorado"); return; }

  String oldCfg = daysStr() + "|" + startsStr() + "|" + minsStr();

  JsonArray dy = d["days"];
  for (int i = 0; i < 7 && i < (int)dy.size(); i++) days[i] = dy[i].as<bool>();
  JsonArray z = d["zones"];
  for (int i = 0; i < NZ && i < (int)z.size(); i++) zoneMin[i] = constrain(z[i].as<int>(), 0, 240);
  String s;
  for (JsonVariant v : d["starts"].as<JsonArray>()) { if (s.length()) s += ","; s += v.as<String>(); }
  parseStarts(s);

  if (daysStr() + "|" + startsStr() + "|" + minsStr() != oldCfg) {
    struct tm t;
    savedAt = timeValid(t) ? two(t.tm_mday) + "/" + two(t.tm_mon + 1) + " " + two(t.tm_hour) + ":" + two(t.tm_min) : "";
    saveCfg();
    addEv("Programa guardado: días " + daysNice() + " · horas " + (nStarts ? startsStr() : "ninguna") + " · min " + minsStr());
  }
  dirty = true;
}

void keepMqtt() {
  static bool wasWifi = false;
  bool wifi = WiFi.status() == WL_CONNECTED;
  if (wifi != wasWifi) { wasWifi = wifi; addEv(wifi ? "WiFi conectado (" + WiFi.localIP().toString() + ")" : "WiFi perdido"); }
  if (mqtt.connected() || !wifi || millis() - lastTry < 5000) return;
  lastTry = millis();
  String id = "riego-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (mqtt.connect(id.c_str(), MQTT_USER, MQTT_PASS, topic("online").c_str(), 1, true, "0")) {
    mqtt.publish(topic("online").c_str(), "1", true);
    mqtt.subscribe(topic("config").c_str(), 1);
    mqtt.subscribe(topic("cmd").c_str(), 1);
    addEv("Conectado al servidor MQTT");
    dirty = true;
    logDirty = true;
  } else {
    Serial.printf("MQTT error %d\n", mqtt.state());
  }
}

void setup() {
  for (int i = 0; i < NZ; i++) { setRelay(i, false); pinMode(RELAY_PINS[i], OUTPUT); setRelay(i, false); }
  Serial.begin(115200);
  prefs.begin("riego", false);
  loadCfg();
  addEv("ESP32 encendido");
  addEv("Programa cargado: días " + daysNice() + " · horas " + (nStarts ? startsStr() : "ninguna") + " · min " + minsStr());

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  if (WiFi.status() != WL_CONNECTED) addEv("Sin WiFi: sigo con el programa guardado");
  sntp_set_sync_interval(3600000UL);  // sincroniza con internet cada hora
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");

  net.setInsecure();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(2048);
  mqtt.setKeepAlive(30);
  mqtt.setCallback(onMessage);
}

void loop() {
  tickClock();
  keepMqtt();
  mqtt.loop();

  if (running && clk() - zoneStartMs >= (uint64_t)runDur[runIdx] * 60000ULL) nextZone();

  struct tm t;
  if (timeValid(t)) {
    if (!timeOk) { timeOk = true; addEv("Hora sincronizada"); }
    int m = t.tm_hour * 60 + t.tm_min;
    if (m != lastMinute) {
      lastMinute = m;
      int wd = (t.tm_wday + 6) % 7;
      for (int i = 0; i < nStarts; i++) {
        if (starts[i] != m) continue;
        if (days[wd]) startCycle("programado " + hhmm(m));
        else addEv("Hora " + hhmm(m) + ": hoy no toca regar");
      }
    }
  }

  if (mqtt.connected()) {
    if (dirty) publishState();
    if (logDirty) publishLog();
    if (millis() - lastClockPub >= CLOCK_EVERY) { lastClockPub = millis(); publishClock(); }
  }
  delay(5);
}
