/* ============================================================
   PlantGuard · firmware ESP32 (Nivel 2: hardware real)
   Equipo Bytec

   - Lee el sensor capacitivo de humedad del suelo y el DHT11/DHT22.
   - Decide el riego AQUÍ, en el ESP32: si se cae el WiFi la planta se sigue regando
     con el último umbral guardado.
   - Publica su estado en Firestore (dispositivos/{ID}) y recibe de la app
     los umbrales (cfg) y las órdenes de la bomba (cmd).

   Proyecto PlatformIO: abre la carpeta firmware/ en VS Code. Las librerías
   (DHT de Adafruit y ArduinoJson 7) se instalan solas desde platformio.ini.
   ============================================================ */
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <time.h>
#include <sys/time.h>

#if __has_include("config.h")
#include "config.h"
#else
#error "Falta include/config.h: copia include/config.example.h como include/config.h y escribe tu WiFi."
#endif

#define FW_VERSION "1.1.0"

#ifndef LOCAL_SERVER_URL      /* por si tu config.h es de la versión 1.0.0 */
#define LOCAL_SERVER_URL ""
#endif
#ifndef LOCAL_EVERY_MS
#define LOCAL_EVERY_MS 3000UL
#endif

/* ---------------- Estado compartido entre el control (loop) y la nube (tarea) ---------------- */
struct Cfg { int thr; int dur; uint32_t waitMs; };

struct Ev { uint32_t ms; char type[8]; char msg[96]; };

static SemaphoreHandle_t mtx;
#define LOCK()   xSemaphoreTake(mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(mtx)

static Cfg cfg = { DEFAULT_THR, DEFAULT_DUR, DEFAULT_WAIT_MS };

static float soilPct = 0, tempC = 0, humPct = 0;
static int soilRaw = 0;
static bool soilOk = false, dhtOk = false, haveSoil = false, haveDht = false;
static bool pumpOn = false;
static char pumpMode[8] = "";          /* "auto" | "manual" | "" */
static uint32_t pumpStartMs = 0, pumpForMs = 0;
static uint32_t lastEndMs = 0;         /* fin del último riego AUTOMÁTICO (millis) */
static bool hasLastEnd = false;
static bool critFlag = false;

static int pendingCmd = 0;             /* 0 nada · 1 encender · 2 apagar (lo pone la nube, lo aplica el loop) */
static int64_t pendingCmdId = 0, cmdAck = 0;
static volatile bool stateDirty = true;
static volatile bool cloudOk = false;

#define EVQ 16
static Ev evq[EVQ];
static int evHead = 0, evCount = 0;

static char deviceId[32];
static Preferences prefs;
static DHT dht(PIN_DHT, DHT_TYPE);

/* ---------------- Utilidades ---------------- */
static int64_t epochMs() {               /* 0 si todavía no hay hora de internet (NTP) */
  struct timeval tv; gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1700000000) return 0;
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
static int64_t msToEpoch(uint32_t ms) {  /* convierte un instante de millis() a hora real */
  int64_t e = epochMs(); if (!e) return 0;
  return e - (int64_t)(uint32_t)(millis() - ms);
}
static float r1(float x) { return roundf(x * 10) / 10; }

static void pushEvent(const char* type, const char* msg) {   /* llamar con LOCK tomado */
  int i = (evHead + evCount) % EVQ;
  if (evCount == EVQ) { evHead = (evHead + 1) % EVQ; evCount--; }  /* cola llena: se pierde el más viejo */
  evq[i].ms = millis();
  strncpy(evq[i].type, type, sizeof(evq[i].type) - 1); evq[i].type[sizeof(evq[i].type) - 1] = 0;
  strncpy(evq[i].msg, msg, sizeof(evq[i].msg) - 1); evq[i].msg[sizeof(evq[i].msg) - 1] = 0;
  evCount++;
  Serial.printf("[evento] %s · %s\n", type, msg);
}

/* ---------------- Bomba ---------------- */
static void relay(bool on) { digitalWrite(PIN_RELAY, (on != RELAY_ACTIVE_LOW) ? HIGH : LOW); }

static void startPump(const char* mode, int seconds) {        /* con LOCK */
  if (pumpOn) return;
  if (seconds > PUMP_MAX_S) seconds = PUMP_MAX_S;
  if (seconds < 1) seconds = 1;
  pumpOn = true; relay(true);
  strncpy(pumpMode, mode, sizeof(pumpMode) - 1);
  pumpStartMs = millis(); pumpForMs = (uint32_t)seconds * 1000;
  char m[96];
  snprintf(m, sizeof m, "%s · humedad al %d%%",
           strcmp(mode, "auto") == 0 ? "Riego automático activado" : "Riego manual iniciado", (int)roundf(soilPct));
  pushEvent(mode, m);
  stateDirty = true;
}

static void stopPump() {                                       /* con LOCK */
  if (!pumpOn) return;
  relay(false); pumpOn = false;
  int sec = (int)roundf((millis() - pumpStartMs) / 1000.0f);
  if (strcmp(pumpMode, "auto") == 0) { lastEndMs = millis(); hasLastEnd = true; }  /* el manual no frena al automático */
  char m[96];
  snprintf(m, sizeof m, "Riego terminado · duró %d s (humedad al %d%%)", sec, (int)roundf(soilPct));
  pushEvent("stop", m);
  stateDirty = true;
}

/* ---------------- Sensores ---------------- */
static int readSoilRaw() {               /* promedio de 32 muestras quitando la mínima y la máxima */
  long sum = 0; int mn = 4095, mx = 0;
  for (int i = 0; i < 32; i++) {
    int v = analogRead(PIN_SOIL);
    sum += v; if (v < mn) mn = v; if (v > mx) mx = v;
    delayMicroseconds(300);
  }
  return (int)((sum - mn - mx) / 30);
}

static float rawToPct(int raw) {         /* más agua = valor más BAJO en el sensor capacitivo */
  float p = (float)(SOIL_RAW_DRY - raw) * 100.0f / (float)(SOIL_RAW_DRY - SOIL_RAW_WET);
  return p < 0 ? 0 : p > 100 ? 100 : p;
}

static void soilStep() {
  static int bad = 0, good = 0;
  int raw = readSoilRaw();
  bool valid = raw >= SOIL_RAW_MIN_VALID && raw <= SOIL_RAW_MAX_VALID;
  float pct = rawToPct(raw);

  LOCK();
  soilRaw = raw;
  if (valid) {
    good++; bad = 0;
    soilPct = haveSoil ? soilPct * 0.6f + pct * 0.4f : pct;   /* suavizado para que no "brinque" */
    haveSoil = true;
    if (!soilOk && good >= 3) {
      soilOk = true; stateDirty = true;
      static bool first = true;
      if (!first) pushEvent("on", "Sensor de suelo reconectado");
      first = false;
    }
  } else {
    bad++; good = 0;
    if (soilOk && bad >= 3) {
      soilOk = false; stateDirty = true;
      pushEvent("off", "Sensor de suelo desconectado");
      if (pumpOn && strcmp(pumpMode, "auto") == 0) stopPump();  /* sin lectura no se riega a ciegas */
    }
  }

  if (soilOk) {
    bool crit = soilPct < cfg.thr - 10;
    if (crit && !critFlag) {
      char m[96]; snprintf(m, sizeof m, "Humedad crítica detectada · %d%% en el suelo", (int)roundf(soilPct));
      pushEvent("crit", m);
    }
    critFlag = crit;
    bool esperaOk = !hasLastEnd || (uint32_t)(millis() - lastEndMs) >= cfg.waitMs;
    if (!pumpOn && soilPct < cfg.thr && esperaOk) startPump("auto", cfg.dur);
  }
  UNLOCK();

  Serial.printf("suelo raw=%4d -> %5.1f%% %s | %.1f°C %.0f%% %s | bomba %s\n",
                raw, pct, valid ? "" : "(FUERA DE RANGO)", tempC, humPct, dhtOk ? "" : "(DHT sin lectura)",
                pumpOn ? pumpMode : "apagada");
}

static void dhtStep() {
  static int bad = 0;
  float t = dht.readTemperature(), h = dht.readHumidity();
  LOCK();
  if (!isnan(t) && !isnan(h)) {
    tempC = t; humPct = h; haveDht = true; bad = 0;
    if (!dhtOk) { dhtOk = true; stateDirty = true; }
  } else if (++bad >= 3 && dhtOk) {
    dhtOk = false; stateDirty = true;
    pushEvent("off", "Sensor de temperatura y humedad (DHT) sin respuesta");
  }
  UNLOCK();
}

/* ---------------- Nube: HTTP + Firestore REST ---------------- */
static String idToken, refreshToken;
static uint32_t tokenExpMs = 0;
static int64_t lastCmdId = 0;
static const String FS_DOC = String("https://firestore.googleapis.com/v1/projects/") + FIREBASE_PROJECT_ID +
                             "/databases/(default)/documents/dispositivos/";

static int http(const char* method, const String& url, const String& body, String& resp,
                const char* ctype = "application/json", bool auth = true) {
  WiFiClientSecure client;
  client.setInsecure();   /* prototipo escolar: no se valida el certificado. Ver README → Seguridad */
  HTTPClient h;
  h.setConnectTimeout(8000); h.setTimeout(8000);
  if (!h.begin(client, url)) return -1;
  h.addHeader("Content-Type", ctype);
  if (auth) h.addHeader("Authorization", "Bearer " + idToken);
  int code = h.sendRequest(method, body);
  resp = code > 0 ? h.getString() : String();
  h.end();
  if (code < 200 || code >= 300) {
    Serial.printf("[nube] %s %s -> %d %s\n", method, url.c_str(), code, resp.substring(0, 200).c_str());
    if (code == 401 || code == 403) tokenExpMs = 0;   /* fuerza a renovar la sesión */
  }
  return code;
}

static bool saveTokens(JsonDocument& d, const char* idKey, const char* rfKey, const char* expKey) {
  const char* id = d[idKey] | "";
  const char* rf = d[rfKey] | "";
  if (!*id || !*rf) return false;
  idToken = id; refreshToken = rf;
  long exp = atol(d[expKey] | "3600");
  tokenExpMs = millis() + (uint32_t)(exp > 300 ? exp - 300 : exp) * 1000;  /* renueva 5 min antes */
  prefs.putString("rtok", refreshToken);
  return true;
}

static bool ensureToken() {
  if (idToken.length() && tokenExpMs && (int32_t)(tokenExpMs - millis()) > 0) return true;
  String resp; JsonDocument d;
  if (refreshToken.length()) {    /* reutiliza la misma sesión anónima para no crear usuarios nuevos en cada reinicio */
    String body = "grant_type=refresh_token&refresh_token=" + refreshToken;
    int c = http("POST", String("https://securetoken.googleapis.com/v1/token?key=") + FIREBASE_API_KEY, body, resp,
                 "application/x-www-form-urlencoded", false);
    if (c == 200 && !deserializeJson(d, resp) && saveTokens(d, "id_token", "refresh_token", "expires_in")) return true;
    if (c != 400) return false;   /* error de red: reintenta luego. 400 = token inválido: crea sesión nueva */
  }
  int c = http("POST", String("https://identitytoolkit.googleapis.com/v1/accounts:signUp?key=") + FIREBASE_API_KEY,
               "{\"returnSecureToken\":true}", resp, "application/json", false);
  d.clear();
  if (c == 200 && !deserializeJson(d, resp) && saveTokens(d, "idToken", "refreshToken", "expiresIn")) {
    Serial.println("[nube] sesión anónima creada");
    return true;
  }
  Serial.println("[nube] no se pudo iniciar sesión. ¿Está activado el acceso Anónimo en Firebase Authentication?");
  return false;
}

/* Firestore manda los números como {"integerValue":"35"} o {"doubleValue":35.5} */
static double fsNum(JsonVariantConst v, double def) {
  if (v.isNull()) return def;
  JsonVariantConst i = v["integerValue"];
  if (!i.isNull()) return i.is<const char*>() ? (double)strtoll(i.as<const char*>(), nullptr, 10) : i.as<double>();
  JsonVariantConst d = v["doubleValue"];
  if (!d.isNull()) return d.as<double>();
  return def;
}
static int64_t fsInt(JsonVariantConst v) {
  JsonVariantConst i = v["integerValue"];
  if (!i.isNull()) return i.is<const char*>() ? strtoll(i.as<const char*>(), nullptr, 10) : (int64_t)i.as<double>();
  return (int64_t)fsNum(v, 0);
}
static void putI(JsonObject o, const char* k, int64_t v) { char b[24]; snprintf(b, sizeof b, "%lld", (long long)v); o[k]["integerValue"] = String(b); }
static void putD(JsonObject o, const char* k, double v) { o[k]["doubleValue"] = v; }
static void putB(JsonObject o, const char* k, bool v) { o[k]["booleanValue"] = v; }
static void putS(JsonObject o, const char* k, const char* v) { o[k]["stringValue"] = String(v); }
static void putNull(JsonObject o, const char* k) { o[k]["nullValue"] = nullptr; }

/* Lee de la app: cfg { thr, dur, waitMs } y cmd { id, accion: "on" | "off" } */
static bool pollDevice() {
  String resp;
  int c = http("GET", FS_DOC + deviceId + "?mask.fieldPaths=cfg&mask.fieldPaths=cmd", "", resp);
  if (c == 404) return true;              /* el documento aún no existe: se crea con el primer estado */
  if (c != 200) return false;
  JsonDocument d;
  if (deserializeJson(d, resp)) return false;

  JsonVariantConst cf = d["fields"]["cfg"]["mapValue"]["fields"];
  if (!cf.isNull()) {
    Cfg n;
    n.thr = constrain((int)fsNum(cf["thr"], cfg.thr), 5, 95);
    n.dur = constrain((int)fsNum(cf["dur"], cfg.dur), 1, PUMP_MAX_S);
    n.waitMs = (uint32_t)constrain(fsNum(cf["waitMs"], cfg.waitMs), 1000.0, 86400000.0);
    if (n.thr != cfg.thr || n.dur != cfg.dur || n.waitMs != cfg.waitMs) {
      LOCK(); cfg = n; stateDirty = true; UNLOCK();
      prefs.putInt("thr", n.thr); prefs.putInt("dur", n.dur); prefs.putUInt("wait", n.waitMs);
      Serial.printf("[nube] umbrales nuevos: %d%% · %d s · espera %lu s\n", n.thr, n.dur, (unsigned long)(n.waitMs / 1000));
    }
  }

  JsonVariantConst cm = d["fields"]["cmd"]["mapValue"]["fields"];
  if (!cm.isNull()) {
    int64_t id = fsInt(cm["id"]);
    const char* acc = cm["accion"]["stringValue"] | "";
    if (id && id != lastCmdId) {
      lastCmdId = id;
      int64_t age = epochMs() - id;
      if (age > -60000 && age < 60000) {   /* ignora órdenes viejas (p. ej. al reiniciar el ESP32) */
        LOCK();
        pendingCmd = strcmp(acc, "on") == 0 ? 1 : strcmp(acc, "off") == 0 ? 2 : 0;
        pendingCmdId = id;
        UNLOCK();
      }
    }
  }
  return true;
}

static bool sendState() {
  JsonDocument d;
  JsonObject e = d["fields"]["estado"]["mapValue"]["fields"].to<JsonObject>();
  LOCK();
  putI(e, "t", epochMs());
  putD(e, "soil", r1(soilPct)); putI(e, "soilRaw", soilRaw); putB(e, "soilOk", soilOk);
  if (haveDht) { putD(e, "temp", r1(tempC)); putD(e, "hum", roundf(humPct)); } else { putNull(e, "temp"); putNull(e, "hum"); }
  putB(e, "dhtOk", dhtOk);
  putB(e, "pump", pumpOn); putS(e, "pumpMode", pumpOn ? pumpMode : "");
  int32_t remMs = pumpOn ? (int32_t)pumpForMs - (int32_t)(millis() - pumpStartMs) : 0;
  putI(e, "pumpLeft", remMs > 0 ? (remMs + 999) / 1000 : 0);
  putI(e, "lastEnd", hasLastEnd ? msToEpoch(lastEndMs) : 0);
  putI(e, "cmdAck", cmdAck);
  putI(e, "thr", cfg.thr); putI(e, "dur", cfg.dur); putI(e, "waitMs", cfg.waitMs);
  stateDirty = false;
  UNLOCK();
  putI(e, "rssi", WiFi.RSSI()); putS(e, "ip", WiFi.localIP().toString().c_str());
  putS(e, "fw", FW_VERSION); putI(e, "uptime", millis() / 1000);

  String body, resp; serializeJson(d, body);
  int c = http("PATCH", FS_DOC + deviceId + "?updateMask.fieldPaths=estado", body, resp);
  if (c != 200) stateDirty = true;
  return c == 200;
}

static bool sendReading() {
  JsonDocument d; JsonObject f = d["fields"].to<JsonObject>();
  LOCK();
  bool ok = soilOk;
  putI(f, "t", epochMs()); putD(f, "soil", r1(soilPct));
  if (haveDht) { putD(f, "temp", r1(tempC)); putI(f, "hum", (int)roundf(humPct)); }
  UNLOCK();
  if (!ok) return true;                    /* sin sensor de suelo no se guarda lectura (igual que en la simulación) */
  String body, resp; serializeJson(d, body);
  return http("POST", FS_DOC + deviceId + "/lecturas", body, resp) == 200;
}

static bool drainEvents() {
  for (;;) {
    Ev ev;
    LOCK();
    if (!evCount) { UNLOCK(); return true; }
    ev = evq[evHead];
    UNLOCK();
    JsonDocument d; JsonObject f = d["fields"].to<JsonObject>();
    putI(f, "t", msToEpoch(ev.ms)); putS(f, "type", ev.type); putS(f, "msg", ev.msg);
    String body, resp; serializeJson(d, body);
    if (http("POST", FS_DOC + deviceId + "/eventos", body, resp) != 200) return false;
    LOCK();
    if (evCount && evq[evHead].ms == ev.ms && strcmp(evq[evHead].type, ev.type) == 0) { evHead = (evHead + 1) % EVQ; evCount--; }
    UNLOCK();
  }
}

/* Servidor local opcional (el código anterior del equipo): POST {"humedad": raw, ...} */
static void sendLocal() {
  JsonDocument d;
  LOCK();
  d["humedad"] = soilRaw;
  d["humedadPct"] = r1(soilPct);
  d["soilOk"] = soilOk;
  if (haveDht) { d["temp"] = r1(tempC); d["hum"] = (int)roundf(humPct); }
  d["bomba"] = pumpOn;
  d["id"] = String(deviceId);
  UNLOCK();
  String body; serializeJson(d, body);
  WiFiClient c; HTTPClient h;
  h.setConnectTimeout(2000); h.setTimeout(2000);
  if (!h.begin(c, LOCAL_SERVER_URL)) return;
  h.addHeader("Content-Type", "application/json");
  int code = h.POST(body);
  if (code < 200 || code >= 300) Serial.printf("[local] %s -> %d (¿está encendido el servidor y es la IP correcta?)\n", LOCAL_SERVER_URL, code);
  h.end();
}

static void cloudTask(void*) {
  uint32_t lastPoll = 0, lastState = 0, lastReading = 0, wifiTry = 0, lastLocal = 0, authRetryAt = 0;
  bool ntp = false, hello = false, polled = false;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      cloudOk = false;
      if (millis() - wifiTry > 20000) {     /* reconexión automática */
        wifiTry = millis();
        Serial.println("[wifi] reconectando…");
        WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASS);
      }
      vTaskDelay(pdMS_TO_TICKS(500)); continue;
    }
    if (strlen(LOCAL_SERVER_URL) && millis() - lastLocal >= LOCAL_EVERY_MS) { lastLocal = millis(); sendLocal(); }
    if (!ntp) { configTime(0, 0, "pool.ntp.org", "time.google.com"); ntp = true; }
    if (!epochMs()) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
    if ((int32_t)(millis() - authRetryAt) < 0) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
    if (!ensureToken()) { cloudOk = false; authRetryAt = millis() + 10000; vTaskDelay(pdMS_TO_TICKS(200)); continue; }

    if (!hello) {
      LOCK(); pushEvent("on", "ESP32 en línea · firmware " FW_VERSION); UNLOCK();
      hello = true;
    }
    bool ok = true;
    uint32_t now = millis();
    if (!polled || now - lastPoll >= POLL_EVERY_MS) { lastPoll = now; ok &= pollDevice(); polled = true; }
    ok &= drainEvents();
    if (stateDirty || now - lastState >= STATE_EVERY_MS) { lastState = now; ok &= sendState(); }
    if (now - lastReading >= READING_EVERY_MS) { lastReading = now; ok &= sendReading(); }
    cloudOk = ok;
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

/* ---------------- Arranque y lazo de control ---------------- */
void setup() {
  relay(false); pinMode(PIN_RELAY, OUTPUT); relay(false);   /* bomba apagada desde el primer instante */
  pinMode(PIN_LED, OUTPUT);
  Serial.begin(115200); delay(200);
  mtx = xSemaphoreCreateMutex();

  if (strlen(DEVICE_ID)) strncpy(deviceId, DEVICE_ID, sizeof(deviceId) - 1);
  else {
    uint64_t mac = ESP.getEfuseMac();
    snprintf(deviceId, sizeof deviceId, "pg-%02x%02x%02x",
             (unsigned)((mac >> 24) & 0xff), (unsigned)((mac >> 32) & 0xff), (unsigned)((mac >> 40) & 0xff));
  }
  Serial.println("\n========================================");
  Serial.printf(" PlantGuard ESP32 · firmware %s\n", FW_VERSION);
  Serial.printf(" ID del dispositivo: %s\n", deviceId);
  Serial.println(" (escríbelo en la app: Mi planta → Editar → ID del ESP32)");
  Serial.println("========================================");

  prefs.begin("pg", false);   /* los últimos umbrales sobreviven a un reinicio sin internet */
  cfg.thr = prefs.getInt("thr", DEFAULT_THR);
  cfg.dur = prefs.getInt("dur", DEFAULT_DUR);
  cfg.waitMs = prefs.getUInt("wait", DEFAULT_WAIT_MS);
  refreshToken = prefs.getString("rtok", "");

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_SOIL, ADC_11db);   /* rango completo 0-3.3 V */
  dht.begin();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  xTaskCreatePinnedToCore(cloudTask, "nube", 16384, nullptr, 1, nullptr, 0);
}

void loop() {
  static uint32_t tSoil = 0, tDht = 0;
  uint32_t now = millis();

  if (now - tSoil >= 2000) { tSoil = now; soilStep(); }
  if (now - tDht >= 3000) { tDht = now; dhtStep(); }

  LOCK();
  if (pendingCmd) {                         /* orden de la app */
    if (pendingCmd == 1) startPump("manual", MANUAL_S);
    else stopPump();
    cmdAck = pendingCmdId; pendingCmd = 0; stateDirty = true;
  }
  if (pumpOn && (uint32_t)(millis() - pumpStartMs) >= pumpForMs) stopPump();   /* fin del riego por tiempo */
  UNLOCK();

  digitalWrite(PIN_LED, cloudOk ? HIGH : ((millis() / 400) % 2 ? HIGH : LOW));
  delay(20);
}
