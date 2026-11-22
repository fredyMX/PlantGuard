/* ============================================================
   PlantGuard · firmware ESP32 (Nivel 2: hardware real)
   Equipo Bytec

   - Hasta 2 CANALES independientes (una planta cada uno): sensor de humedad del suelo
     (FC-28 / MH / HW-080 o capacitivo) + bomba opcional. El DHT11/DHT22 es compartido.
   - Cada canal decide su riego AQUÍ, en el ESP32 (si se cae el WiFi se sigue regando):
       · humedad < umbral mínimo              → se ENCIENDE la bomba
       · humedad ≥ umbral + HUMEDAD_RANGO     → se APAGA (entre ambos conserva su estado)
       · encendida más del tiempo máximo      → se APAGA y entra en PAUSA DE SEGURIDAD;
                                                 al terminar la pausa vuelve a intentar
     Umbral, tiempo máximo y pausa se cambian desde la app (pantalla Umbrales).
   - Cada canal es un "dispositivo" en Firestore: canal 1 = dispositivos/{ID},
     canal 2 = dispositivos/{ID}-2. En la app, cada planta usa uno de esos IDs.

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

#define FW_VERSION "1.4.2"

#ifndef LOCAL_SERVER_URL /* por si tu config.h es de la versión 1.0.0 */
#define LOCAL_SERVER_URL ""
#endif
#ifndef LOCAL_EVERY_MS
#define LOCAL_EVERY_MS 3000UL
#endif
#ifndef HUMEDAD_RANGO
#define HUMEDAD_RANGO 20 /* la bomba se apaga al llegar a umbral + 20 % (40 → 60 %) */
#endif
/* Actualización rápida (valores por defecto si tu config.h no los trae) */
#ifndef SOIL_EVERY_MS
#define SOIL_EVERY_MS 1000UL /* leer el sensor de suelo cada 1 s */
#endif
#ifndef STATE_MIN_MS
#define STATE_MIN_MS 2000UL /* como máximo un envío cada 2 s cuando la humedad cambia */
#endif
#ifndef SOIL_SEND_DELTA
#define SOIL_SEND_DELTA 2.0f /* cuántos puntos de % tiene que cambiar para enviar de inmediato */
#endif

/* "" + la URL: así compila aunque en config.h quede  #define LOCAL_SERVER_URL  sin nada (o comentado) */
static const char *const LOCAL_URL = "" LOCAL_SERVER_URL;

/* ---------------- Canales: compatibilidad con config.h de versiones anteriores ---------------- */
#ifndef NUM_CANALES
#define NUM_CANALES 1
#endif
#if NUM_CANALES < 1 || NUM_CANALES > 2
#error "NUM_CANALES debe ser 1 o 2"
#endif
#ifndef CH1_SOIL_PIN
#define CH1_SOIL_PIN PIN_SOIL
#endif
#ifndef CH1_RELAY_PIN
#define CH1_RELAY_PIN PIN_RELAY
#endif
#ifndef CH1_RELAY_ACTIVE_LOW
#define CH1_RELAY_ACTIVE_LOW RELAY_ACTIVE_LOW
#endif
#ifndef CH1_RAW_DRY
#define CH1_RAW_DRY SOIL_RAW_DRY
#endif
#ifndef CH1_RAW_WET
#define CH1_RAW_WET SOIL_RAW_WET
#endif
#ifndef CH1_RAW_MIN_VALID
#define CH1_RAW_MIN_VALID SOIL_RAW_MIN_VALID
#endif
#ifndef CH1_RAW_MAX_VALID
#define CH1_RAW_MAX_VALID SOIL_RAW_MAX_VALID
#endif
#ifndef CH2_SOIL_PIN
#define CH2_SOIL_PIN 35
#endif
#ifndef CH2_RELAY_PIN
#define CH2_RELAY_PIN -1 /* -1 = sin bomba (solo monitoreo) */
#endif
#ifndef CH2_RELAY_ACTIVE_LOW
#define CH2_RELAY_ACTIVE_LOW CH1_RELAY_ACTIVE_LOW
#endif
#ifndef CH2_RAW_DRY
#define CH2_RAW_DRY CH1_RAW_DRY
#endif
#ifndef CH2_RAW_WET
#define CH2_RAW_WET CH1_RAW_WET
#endif
#ifndef CH2_RAW_MIN_VALID
#define CH2_RAW_MIN_VALID CH1_RAW_MIN_VALID
#endif
#ifndef CH2_RAW_MAX_VALID
#define CH2_RAW_MAX_VALID CH1_RAW_MAX_VALID
#endif

/* ---------------- Estado compartido entre el control (loop) y la nube (tarea) ---------------- */
struct Cfg
{
  int thr;
  int dur;
  uint32_t waitMs;
};

struct Ev
{
  uint32_t ms;
  uint8_t c;
  char type[8];
  char msg[96];
};

/* Un canal = un sensor de suelo + (opcional) una bomba = una planta en la app */
struct Canal
{
  /* conexiones y calibración (de config.h) */
  int soilPin, relayPin;
  bool activeLow;
  int rawDry, rawWet, rawMin, rawMax;
  char id[40]; /* documento en Firestore: dispositivos/{id} */
  Cfg cfg;
  /* lecturas */
  float soilPct;
  int soilRaw;
  bool soilOk, haveSoil, sensorFirst;
  int bad, good;
  /* bomba y riego */
  bool pumpOn;
  char pumpMode[8];
  uint32_t pumpStartMs, pumpForMs;
  uint32_t lastEndMs;
  bool hasLastEnd; /* inicio de la última PAUSA DE SEGURIDAD */
  bool critFlag, pausaAntes;
  /* órdenes de la app y envío */
  int pendingCmd;
  int64_t pendingCmdId, cmdAck, lastCmdId;
  volatile bool stateDirty;
  float sentSoil;
  uint32_t lastState;
};

static SemaphoreHandle_t mtx;
#define LOCK() xSemaphoreTake(mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(mtx)

static Canal ch[NUM_CANALES];
static float tempC = 0, humPct = 0;
static bool dhtOk = false, haveDht = false;
static volatile bool cloudOk = false;

#define EVQ 24
static Ev evq[EVQ];
static int evHead = 0, evCount = 0;

static char deviceId[32];
static Preferences prefs;
static DHT dht(PIN_DHT, DHT_TYPE);

/* ---------------- Utilidades ---------------- */
static int64_t epochMs()
{ /* 0 si todavía no hay hora de internet (NTP) */
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1700000000)
    return 0;
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
static int64_t msToEpoch(uint32_t ms)
{ /* convierte un instante de millis() a hora real */
  int64_t e = epochMs();
  if (!e)
    return 0;
  return e - (int64_t)(uint32_t)(millis() - ms);
}
static float r1(float x) { return roundf(x * 10) / 10; }
static String prefKey(const char *base, int i)
{ /* "thr" para la planta 1, "thr2" para la 2 */
  char b[16];
  snprintf(b, sizeof b, i == 0 ? "%s" : "%s%d", base, i + 1);
  return String(b);
}

static void pushEvent(int c, const char *type, const char *msg)
{ /* llamar con LOCK tomado */
  int i = (evHead + evCount) % EVQ;
  if (evCount == EVQ)
  {
    evHead = (evHead + 1) % EVQ;
    evCount--;
  } /* cola llena: se pierde el más viejo */
  evq[i].ms = millis();
  evq[i].c = (uint8_t)c;
  strncpy(evq[i].type, type, sizeof(evq[i].type) - 1);
  evq[i].type[sizeof(evq[i].type) - 1] = 0;
  strncpy(evq[i].msg, msg, sizeof(evq[i].msg) - 1);
  evq[i].msg[sizeof(evq[i].msg) - 1] = 0;
  evCount++;
  Serial.printf("[evento P%d] %s · %s\n", c + 1, type, msg);
}
static void pushEventAll(const char *type, const char *msg)
{
  for (int c = 0; c < NUM_CANALES; c++)
  {
    pushEvent(c, type, msg);
    ch[c].stateDirty = true;
  }
}

/* ---------------- Bomba ---------------- */
static void controlarBomba(Canal &k, bool encender)
{ /* igual que en tu código de riego */
  if (k.relayPin < 0)
    return; /* canal sin bomba */
  if (k.activeLow)
    digitalWrite(k.relayPin, encender ? LOW : HIGH); /* activo en LOW: LOW = bomba encendida */
  else
    digitalWrite(k.relayPin, encender ? HIGH : LOW); /* activo en HIGH: HIGH = bomba encendida */
}

static int humedadMax(const Canal &k)
{
  int m = k.cfg.thr + HUMEDAD_RANGO;
  return m > 100 ? 100 : m;
}
static bool enPausa(const Canal &k) { return k.hasLastEnd && (uint32_t)(millis() - k.lastEndMs) < k.cfg.waitMs; }
static void fmtSeg(char *b, size_t n, uint32_t ms)
{
  if (ms < 120000)
    snprintf(b, n, "%lu s", (unsigned long)(ms / 1000));
  else
    snprintf(b, n, "%lu min", (unsigned long)(ms / 60000));
}

static void startPump(int c, const char *mode, int seconds)
{ /* con LOCK */
  Canal &k = ch[c];
  if (k.pumpOn || k.relayPin < 0)
    return;
  if (seconds > PUMP_MAX_S)
    seconds = PUMP_MAX_S;
  if (seconds < 1)
    seconds = 1;
  k.pumpOn = true;
  controlarBomba(k, true);
  strncpy(k.pumpMode, mode, sizeof(k.pumpMode) - 1);
  k.pumpMode[sizeof(k.pumpMode) - 1] = 0;
  k.pumpStartMs = millis();
  k.pumpForMs = (uint32_t)seconds * 1000;
  char m[96];
  if (strcmp(mode, "auto") == 0)
    snprintf(m, sizeof m, "Humedad baja (%d%%): bomba encendida · se apaga al llegar a %d%%", (int)roundf(k.soilPct), humedadMax(k));
  else
    snprintf(m, sizeof m, "Riego manual iniciado · humedad al %d%%", (int)roundf(k.soilPct));
  pushEvent(c, mode, m);
  k.stateDirty = true;
}

enum
{
  STOP_META,
  STOP_TIEMPO,
  STOP_APP,
  STOP_SENSOR
};

static void stopPump(int c, int why)
{ /* con LOCK */
  Canal &k = ch[c];
  if (!k.pumpOn)
    return;
  controlarBomba(k, false);
  k.pumpOn = false;
  bool autom = strcmp(k.pumpMode, "auto") == 0;
  int sec = (int)roundf((millis() - k.pumpStartMs) / 1000.0f), h = (int)roundf(k.soilPct);
  char m[96], w[16];
  k.stateDirty = true;
  if (why == STOP_META)
    snprintf(m, sizeof m, "Humedad alcanzada (%d%%): bomba apagada tras %d s", h, sec);
  else if (why == STOP_TIEMPO && autom)
  { /* igual que tu código: corte por tiempo → pausa */
    k.lastEndMs = millis();
    k.hasLastEnd = true;
    fmtSeg(w, sizeof w, k.cfg.waitMs);
    snprintf(m, sizeof m, "Tiempo máximo (%d s) sin llegar al %d%%: pausa de %s (revisa sensor o agua)", sec, humedadMax(k), w);
    pushEvent(c, "pausa", m);
    return;
  }
  else if (why == STOP_TIEMPO)
    snprintf(m, sizeof m, "Riego manual terminado · duró %d s (humedad al %d%%)", sec, h);
  else if (why == STOP_APP)
    snprintf(m, sizeof m, "Bomba apagada desde la app · duró %d s (humedad al %d%%)", sec, h);
  else
    snprintf(m, sizeof m, "Riego detenido: sin lectura del sensor de suelo (duró %d s)", sec);
  pushEvent(c, "stop", m);
}

/* ---------------- Sensores ---------------- */
static int readSoilRaw(int pin)
{ /* promedio de 32 muestras quitando la mínima y la máxima */
  long sum = 0;
  int mn = 4095, mx = 0;
  for (int i = 0; i < 32; i++)
  {
    int v = analogRead(pin);
    sum += v;
    if (v < mn)
      mn = v;
    if (v > mx)
      mx = v;
    delayMicroseconds(300);
  }
  return (int)((sum - mn - mx) / 30);
}

static float rawToPct(const Canal &k, int raw)
{ /* más agua = valor más BAJO (resistivo y capacitivo) */
  float p = (float)(k.rawDry - raw) * 100.0f / (float)(k.rawDry - k.rawWet);
  return p < 0 ? 0 : p > 100 ? 100
                             : p;
}

static void soilStep(int c)
{
  Canal &k = ch[c];
  int raw = readSoilRaw(k.soilPin);
  bool valid = raw >= k.rawMin && raw <= k.rawMax;
  float pct = rawToPct(k, raw);

  LOCK();
  k.soilRaw = raw;
  if (valid)
  {
    k.good++;
    k.bad = 0;
    k.soilPct = k.haveSoil ? k.soilPct * 0.5f + pct * 0.5f : pct; /* suavizado ligero para que no "brinque" */
    k.haveSoil = true;
    if (fabsf(k.soilPct - k.sentSoil) >= SOIL_SEND_DELTA)
      k.stateDirty = true; /* cambió: avisar a la app ya */
    if (!k.soilOk && k.good >= 3)
    {
      k.soilOk = true;
      k.stateDirty = true;
      if (!k.sensorFirst)
        pushEvent(c, "on", "Sensor de suelo reconectado");
      k.sensorFirst = false;
    }
  }
  else
  {
    k.bad++;
    k.good = 0;
    if (k.soilOk && k.bad >= 3)
    {
      k.soilOk = false;
      k.stateDirty = true;
      pushEvent(c, "off", "Sensor de suelo desconectado");
      if (k.pumpOn && strcmp(k.pumpMode, "auto") == 0)
        stopPump(c, STOP_SENSOR); /* sin lectura no se riega a ciegas */
    }
  }

  if (k.soilOk)
  {
    bool crit = k.soilPct < k.cfg.thr - 10;
    if (crit && !k.critFlag)
    {
      char m[96];
      snprintf(m, sizeof m, "Humedad crítica detectada · %d%% en el suelo", (int)roundf(k.soilPct));
      pushEvent(c, "crit", m);
    }
    k.critFlag = crit;
    bool pausa = enPausa(k);
    if (k.pausaAntes && !pausa)
    {
      Serial.printf("[P%d] fin de la pausa de seguridad\n", c + 1);
      k.stateDirty = true;
    }
    k.pausaAntes = pausa;
    if (k.pumpOn && strcmp(k.pumpMode, "auto") == 0 && k.soilPct >= humedadMax(k))
      stopPump(c, STOP_META);
    else if (!k.pumpOn && k.relayPin >= 0 && k.soilPct < k.cfg.thr && !pausa)
      startPump(c, "auto", k.cfg.dur);
  }
  const char *bomba = k.relayPin < 0 ? "sin bomba"
                      : k.pumpOn     ? (strcmp(k.pumpMode, "auto") == 0 ? "ENCENDIDA (auto)" : "ENCENDIDA (manual)")
                      : enPausa(k)   ? "PAUSA DE SEGURIDAD"
                                     : "APAGADA";
  int hMin = k.cfg.thr, hMax = humedadMax(k);
  UNLOCK();

  Serial.printf("[P%d %s] Lectura: %4d | Humedad: %3d %% %s| Bomba: %s | rango %d-%d %%\n",
                c + 1, k.id, raw, (int)roundf(pct), valid ? "" : "(FUERA DE RANGO) ", bomba, hMin, hMax);
}

static void dhtStep()
{
  static int bad = 0;
  float t = dht.readTemperature(), h = dht.readHumidity();
  LOCK();
  if (!isnan(t) && !isnan(h))
  {
    tempC = t;
    humPct = h;
    haveDht = true;
    bad = 0;
    if (!dhtOk)
    {
      dhtOk = true;
      for (int c = 0; c < NUM_CANALES; c++)
        ch[c].stateDirty = true;
    }
  }
  else if (++bad >= 3 && dhtOk)
  {
    dhtOk = false;
    pushEventAll("off", "Sensor de temperatura y humedad (DHT) sin respuesta");
  }
  UNLOCK();
}

/* ---------------- Nube: HTTP + Firestore REST ---------------- */
static String idToken, refreshToken;
static uint32_t tokenExpMs = 0;
static const String FS_DOC = String("https://firestore.googleapis.com/v1/projects/") + FIREBASE_PROJECT_ID +
                             "/databases/(default)/documents/dispositivos/";

/* Conexión segura PERSISTENTE a Firestore: se abre una vez y se reutiliza (keep-alive).
   Abrir una conexión HTTPS nueva en cada envío gasta mucha memoria y "sockets" del ESP32;
   con 2 plantas se agotaban y aparecía "start_ssl_client: -1". */
static WiFiClientSecure fsTls;
static HTTPClient fsHttp;
static bool fsInit = false;
static int fsFails = 0;

static int http(const char *method, const String &url, const String &body, String &resp,
                const char *ctype = "application/json", bool auth = true)
{
  bool firestore = url.startsWith("https://firestore.googleapis.com/");
  int code;
  if (firestore)
  { /* siempre el mismo servidor: se puede reutilizar la conexión */
    if (!fsInit)
    {
      fsTls.setInsecure();
      fsHttp.setReuse(true);
      fsInit = true;
    } /* prototipo escolar: sin validar certificado */
    fsHttp.setConnectTimeout(8000);
    fsHttp.setTimeout(8000);
    if (!fsHttp.begin(fsTls, url))
      return -1;
    fsHttp.addHeader("Content-Type", ctype);
    if (auth)
      fsHttp.addHeader("Authorization", "Bearer " + idToken);
    code = fsHttp.sendRequest(method, body);
    resp = code > 0 ? fsHttp.getString() : String();
    fsHttp.end(); /* con setReuse(true) la conexión queda abierta para el siguiente envío */
    if (code <= 0)
    {
      fsTls.stop();
      fsFails++;
    }
    else
      fsFails = 0; /* conexión rota: la próxima vez se abre una nueva */
  }
  else
  { /* inicio de sesión (otros servidores, muy de vez en cuando) */
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient h;
    h.setConnectTimeout(8000);
    h.setTimeout(8000);
    if (!h.begin(client, url))
      return -1;
    h.addHeader("Content-Type", ctype);
    if (auth)
      h.addHeader("Authorization", "Bearer " + idToken);
    code = h.sendRequest(method, body);
    resp = code > 0 ? h.getString() : String();
    h.end();
  }
  if (code < 200 || code >= 300)
  {
    Serial.printf("[nube] %s %s -> %d %s (memoria libre: %u bytes)\n", method, url.c_str(), code,
                  resp.substring(0, 200).c_str(), (unsigned)ESP.getFreeHeap());
    if (code == 401 || code == 403)
      tokenExpMs = 0; /* fuerza a renovar la sesión */
  }
  return code;
}

static bool saveTokens(JsonDocument &d, const char *idKey, const char *rfKey, const char *expKey)
{
  const char *id = d[idKey] | "";
  const char *rf = d[rfKey] | "";
  if (!*id || !*rf)
    return false;
  idToken = id;
  refreshToken = rf;
  long exp = atol(d[expKey] | "3600");
  tokenExpMs = millis() + (uint32_t)(exp > 300 ? exp - 300 : exp) * 1000; /* renueva 5 min antes */
  prefs.putString("rtok", refreshToken);
  return true;
}

static bool ensureToken()
{
  if (idToken.length() && tokenExpMs && (int32_t)(tokenExpMs - millis()) > 0)
    return true;
  String resp;
  JsonDocument d;
  if (refreshToken.length())
  { /* reutiliza la misma sesión anónima para no crear usuarios nuevos en cada reinicio */
    String body = "grant_type=refresh_token&refresh_token=" + refreshToken;
    int c = http("POST", String("https://securetoken.googleapis.com/v1/token?key=") + FIREBASE_API_KEY, body, resp,
                 "application/x-www-form-urlencoded", false);
    if (c == 200 && !deserializeJson(d, resp) && saveTokens(d, "id_token", "refresh_token", "expires_in"))
      return true;
    if (c != 400)
      return false; /* error de red: reintenta luego. 400 = token inválido: crea sesión nueva */
  }
  int c = http("POST", String("https://identitytoolkit.googleapis.com/v1/accounts:signUp?key=") + FIREBASE_API_KEY,
               "{\"returnSecureToken\":true}", resp, "application/json", false);
  d.clear();
  if (c == 200 && !deserializeJson(d, resp) && saveTokens(d, "idToken", "refreshToken", "expiresIn"))
  {
    Serial.println("[nube] sesión anónima creada");
    return true;
  }
  Serial.println("[nube] no se pudo iniciar sesión. ¿Está activado el acceso Anónimo en Firebase Authentication?");
  return false;
}

/* Firestore manda los números como {"integerValue":"35"} o {"doubleValue":35.5} */
static double fsNum(JsonVariantConst v, double def)
{
  if (v.isNull())
    return def;
  JsonVariantConst i = v["integerValue"];
  if (!i.isNull())
    return i.is<const char *>() ? (double)strtoll(i.as<const char *>(), nullptr, 10) : i.as<double>();
  JsonVariantConst d = v["doubleValue"];
  if (!d.isNull())
    return d.as<double>();
  return def;
}
static int64_t fsInt(JsonVariantConst v)
{
  JsonVariantConst i = v["integerValue"];
  if (!i.isNull())
    return i.is<const char *>() ? strtoll(i.as<const char *>(), nullptr, 10) : (int64_t)i.as<double>();
  return (int64_t)fsNum(v, 0);
}
static void putI(JsonObject o, const char *k, int64_t v)
{
  char b[24];
  snprintf(b, sizeof b, "%lld", (long long)v);
  o[k]["integerValue"] = String(b);
}
static void putD(JsonObject o, const char *k, double v) { o[k]["doubleValue"] = v; }
static void putB(JsonObject o, const char *k, bool v) { o[k]["booleanValue"] = v; }
static void putS(JsonObject o, const char *k, const char *v) { o[k]["stringValue"] = String(v); }
static void putNull(JsonObject o, const char *k) { o[k]["nullValue"] = nullptr; }

/* Lee de la app (para un canal): cfg { thr, dur, waitMs } y cmd { id, accion: "on" | "off" } */
static bool pollDevice(int c)
{
  Canal &k = ch[c];
  String resp;
  int code = http("GET", FS_DOC + k.id + "?mask.fieldPaths=cfg&mask.fieldPaths=cmd", "", resp);
  if (code == 404)
    return true; /* el documento aún no existe: se crea con el primer estado */
  if (code != 200)
    return false;
  JsonDocument d;
  if (deserializeJson(d, resp))
    return false;

  JsonVariantConst cf = d["fields"]["cfg"]["mapValue"]["fields"];
  if (!cf.isNull())
  {
    Cfg n;
    n.thr = constrain((int)fsNum(cf["thr"], k.cfg.thr), 5, 95);
    n.dur = constrain((int)fsNum(cf["dur"], k.cfg.dur), 1, PUMP_MAX_S);
    n.waitMs = (uint32_t)constrain(fsNum(cf["waitMs"], k.cfg.waitMs), 1000.0, 86400000.0);
    if (n.thr != k.cfg.thr || n.dur != k.cfg.dur || n.waitMs != k.cfg.waitMs)
    {
      LOCK();
      k.cfg = n;
      k.stateDirty = true;
      UNLOCK();
      prefs.putInt(prefKey("thr", c).c_str(), n.thr);
      prefs.putInt(prefKey("dur", c).c_str(), n.dur);
      prefs.putUInt(prefKey("wait", c).c_str(), n.waitMs);
      Serial.printf("[P%d] umbrales nuevos: %d%% · %d s · espera %lu s\n", c + 1, n.thr, n.dur, (unsigned long)(n.waitMs / 1000));
    }
  }

  JsonVariantConst cm = d["fields"]["cmd"]["mapValue"]["fields"];
  if (!cm.isNull())
  {
    int64_t id = fsInt(cm["id"]);
    const char *acc = cm["accion"]["stringValue"] | "";
    if (id && id != k.lastCmdId)
    {
      k.lastCmdId = id;
      int64_t age = epochMs() - id;
      if (age > -60000 && age < 60000)
      { /* ignora órdenes viejas (p. ej. al reiniciar el ESP32) */
        LOCK();
        k.pendingCmd = strcmp(acc, "on") == 0 ? 1 : strcmp(acc, "off") == 0 ? 2
                                                                            : 0;
        k.pendingCmdId = id;
        UNLOCK();
      }
    }
  }
  return true;
}

static bool sendState(int c)
{
  Canal &k = ch[c];
  JsonDocument d;
  JsonObject e = d["fields"]["estado"]["mapValue"]["fields"].to<JsonObject>();
  LOCK();
  putI(e, "t", epochMs());
  putD(e, "soil", r1(k.soilPct));
  putI(e, "soilRaw", k.soilRaw);
  putB(e, "soilOk", k.soilOk);
  k.sentSoil = k.soilPct;
  if (haveDht)
  {
    putD(e, "temp", r1(tempC));
    putD(e, "hum", roundf(humPct));
  }
  else
  {
    putNull(e, "temp");
    putNull(e, "hum");
  }
  putB(e, "dhtOk", dhtOk);
  putB(e, "hasPump", k.relayPin >= 0);
  putB(e, "pump", k.pumpOn);
  putS(e, "pumpMode", k.pumpOn ? k.pumpMode : "");
  int32_t remMs = k.pumpOn ? (int32_t)k.pumpForMs - (int32_t)(millis() - k.pumpStartMs) : 0;
  putI(e, "pumpLeft", remMs > 0 ? (remMs + 999) / 1000 : 0);
  putI(e, "lastEnd", k.hasLastEnd ? msToEpoch(k.lastEndMs) : 0); /* inicio de la pausa de seguridad */
  putI(e, "max", humedadMax(k));                                 /* la bomba se apaga al llegar aquí */
  putB(e, "pausa", enPausa(k));
  putI(e, "cmdAck", k.cmdAck);
  putI(e, "thr", k.cfg.thr);
  putI(e, "dur", k.cfg.dur);
  putI(e, "waitMs", k.cfg.waitMs);
  putI(e, "canal", c + 1);
  k.stateDirty = false;
  UNLOCK();
  putI(e, "rssi", WiFi.RSSI());
  putS(e, "ip", WiFi.localIP().toString().c_str());
  putS(e, "fw", FW_VERSION);
  putI(e, "uptime", millis() / 1000);

  String body, resp;
  serializeJson(d, body);
  int code = http("PATCH", FS_DOC + k.id + "?updateMask.fieldPaths=estado", body, resp);
  if (code != 200)
    k.stateDirty = true;
  return code == 200;
}

static bool sendReading(int c)
{
  Canal &k = ch[c];
  JsonDocument d;
  JsonObject f = d["fields"].to<JsonObject>();
  LOCK();
  bool ok = k.soilOk;
  putI(f, "t", epochMs());
  putD(f, "soil", r1(k.soilPct));
  if (haveDht)
  {
    putD(f, "temp", r1(tempC));
    putI(f, "hum", (int)roundf(humPct));
  }
  UNLOCK();
  if (!ok)
    return true; /* sin sensor de suelo no se guarda lectura (igual que en la simulación) */
  String body, resp;
  serializeJson(d, body);
  return http("POST", FS_DOC + k.id + "/lecturas", body, resp) == 200;
}

static bool drainEvents()
{
  for (;;)
  {
    Ev ev;
    LOCK();
    if (!evCount)
    {
      UNLOCK();
      return true;
    }
    ev = evq[evHead];
    UNLOCK();
    JsonDocument d;
    JsonObject f = d["fields"].to<JsonObject>();
    putI(f, "t", msToEpoch(ev.ms));
    putS(f, "type", ev.type);
    putS(f, "msg", ev.msg);
    String body, resp;
    serializeJson(d, body);
    if (http("POST", FS_DOC + ch[ev.c].id + "/eventos", body, resp) != 200)
      return false;
    LOCK();
    if (evCount && evq[evHead].ms == ev.ms && evq[evHead].c == ev.c && strcmp(evq[evHead].type, ev.type) == 0)
    {
      evHead = (evHead + 1) % EVQ;
      evCount--;
    }
    UNLOCK();
  }
}

/* Servidor local opcional (el código anterior del equipo): POST {"humedad": raw del canal 1, ..., "canales": [...]} */
static void sendLocal()
{
  JsonDocument d;
  LOCK();
  d["humedad"] = ch[0].soilRaw; /* igual que antes: el valor crudo del sensor 1 */
  d["humedadPct"] = r1(ch[0].soilPct);
  d["soilOk"] = ch[0].soilOk;
  if (haveDht)
  {
    d["temp"] = r1(tempC);
    d["hum"] = (int)roundf(humPct);
  }
  d["bomba"] = ch[0].pumpOn;
  d["id"] = String(ch[0].id);
  JsonArray a = d["canales"].to<JsonArray>();
  for (int c = 0; c < NUM_CANALES; c++)
  {
    JsonObject o = a.add<JsonObject>();
    o["id"] = String(ch[c].id);
    o["humedad"] = ch[c].soilRaw;
    o["humedadPct"] = r1(ch[c].soilPct);
    o["soilOk"] = ch[c].soilOk;
    o["bomba"] = ch[c].pumpOn;
  }
  UNLOCK();
  String body;
  serializeJson(d, body);
  WiFiClient cl;
  HTTPClient h;
  h.setConnectTimeout(2000);
  h.setTimeout(2000);
  if (!h.begin(cl, LOCAL_URL))
    return;
  h.addHeader("Content-Type", "application/json");
  int code = h.POST(body);
  if (code < 200 || code >= 300)
    Serial.printf("[local] %s -> %d (¿está encendido el servidor y es la IP correcta?)\n", LOCAL_URL, code);
  h.end();
}

static void cloudTask(void *)
{
  uint32_t lastPoll = 0, lastReading = 0, wifiTry = 0, lastLocal = 0, authRetryAt = 0;
  bool ntp = false, hello = false, polled = false;
  for (;;)
  {
    if (WiFi.status() != WL_CONNECTED)
    {
      cloudOk = false;
      if (millis() - wifiTry > 20000)
      { /* reconexión automática */
        wifiTry = millis();
        Serial.println("[wifi] reconectando…");
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASS);
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (LOCAL_URL[0] && millis() - lastLocal >= LOCAL_EVERY_MS)
    {
      lastLocal = millis();
      sendLocal();
    }
    if (!ntp)
    {
      configTime(0, 0, "pool.ntp.org", "time.google.com");
      ntp = true;
    }
    if (!epochMs())
    {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if ((int32_t)(millis() - authRetryAt) < 0)
    {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    if (!ensureToken())
    {
      cloudOk = false;
      authRetryAt = millis() + 10000;
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    if (!hello)
    {
      LOCK();
      pushEventAll("on", "ESP32 en línea · firmware " FW_VERSION);
      UNLOCK();
      hello = true;
    }
    bool ok = true;
    uint32_t now = millis();
    if (!polled || now - lastPoll >= POLL_EVERY_MS)
    {
      lastPoll = now;
      polled = true;
      for (int c = 0; c < NUM_CANALES; c++)
        ok &= pollDevice(c);
    }
    ok &= drainEvents();
    for (int c = 0; c < NUM_CANALES; c++)
    {
      Canal &k = ch[c];
      now = millis();
      if ((k.stateDirty && now - k.lastState >= STATE_MIN_MS) || now - k.lastState >= STATE_EVERY_MS)
      {
        k.lastState = now;
        ok &= sendState(c);
      }
    }
    if (millis() - lastReading >= READING_EVERY_MS)
    {
      lastReading = millis();
      for (int c = 0; c < NUM_CANALES; c++)
        ok &= sendReading(c);
    }
    cloudOk = ok;
    vTaskDelay(pdMS_TO_TICKS(ok ? 200 : (fsFails > 3 ? 10000 : 2000))); /* si falla la red, espera antes de reintentar */
  }
}

/* ---------------- Arranque y lazo de control ---------------- */
static void initCanal(int c, int soilPin, int relayPin, bool activeLow, int dry, int wet, int mn, int mx)
{
  Canal &k = ch[c];
  memset(&k, 0, sizeof k);
  k.soilPin = soilPin;
  k.relayPin = relayPin;
  k.activeLow = activeLow;
  k.rawDry = dry;
  k.rawWet = wet;
  k.rawMin = mn;
  k.rawMax = mx;
  k.sensorFirst = true;
  k.sentSoil = -100;
  k.stateDirty = true;
  if (relayPin >= 0)
  {
    controlarBomba(k, false);
    pinMode(relayPin, OUTPUT);
    controlarBomba(k, false);
  } /* bomba apagada desde el primer instante */
}

void setup()
{
  initCanal(0, CH1_SOIL_PIN, CH1_RELAY_PIN, CH1_RELAY_ACTIVE_LOW, CH1_RAW_DRY, CH1_RAW_WET, CH1_RAW_MIN_VALID, CH1_RAW_MAX_VALID);
#if NUM_CANALES > 1
  initCanal(1, CH2_SOIL_PIN, CH2_RELAY_PIN, CH2_RELAY_ACTIVE_LOW, CH2_RAW_DRY, CH2_RAW_WET, CH2_RAW_MIN_VALID, CH2_RAW_MAX_VALID);
#endif
  pinMode(PIN_LED, OUTPUT);
  Serial.begin(115200);
  delay(200);
  mtx = xSemaphoreCreateMutex();

  if (strlen(DEVICE_ID))
    strncpy(deviceId, DEVICE_ID, sizeof(deviceId) - 1);
  else
  {
    uint64_t mac = ESP.getEfuseMac();
    snprintf(deviceId, sizeof deviceId, "pg-%02x%02x%02x",
             (unsigned)((mac >> 24) & 0xff), (unsigned)((mac >> 32) & 0xff), (unsigned)((mac >> 40) & 0xff));
  }
  prefs.begin("pg", false); /* los últimos umbrales sobreviven a un reinicio sin internet */
  refreshToken = prefs.getString("rtok", "");
  analogReadResolution(12);

  Serial.println("\n========================================");
  Serial.printf(" PlantGuard ESP32 · firmware %s · %d planta(s)\n", FW_VERSION, NUM_CANALES);
  for (int c = 0; c < NUM_CANALES; c++)
  {
    Canal &k = ch[c];
    if (c == 0)
      snprintf(k.id, sizeof k.id, "%s", deviceId);
    else
      snprintf(k.id, sizeof k.id, "%s-%d", deviceId, c + 1);
    k.cfg.thr = prefs.getInt(prefKey("thr", c).c_str(), DEFAULT_THR);
    k.cfg.dur = prefs.getInt(prefKey("dur", c).c_str(), DEFAULT_DUR);
    k.cfg.waitMs = prefs.getUInt(prefKey("wait", c).c_str(), DEFAULT_WAIT_MS);
    analogSetPinAttenuation(k.soilPin, ADC_11db); /* rango completo 0-3.3 V */
    if (k.relayPin >= 0)
      Serial.printf(" Planta %d → ID: %s   (sensor GPIO%d · bomba GPIO%d, se enciende con %s)\n",
                    c + 1, k.id, k.soilPin, k.relayPin, k.activeLow ? "LOW" : "HIGH");
    else
      Serial.printf(" Planta %d → ID: %s   (sensor GPIO%d · sin bomba)\n", c + 1, k.id, k.soilPin);
  }
  Serial.println(" (escribe cada ID en su planta: Mi planta → Editar → ID del ESP32)");
  Serial.printf(" Memoria libre: %u bytes\n", (unsigned)ESP.getFreeHeap());
  Serial.println("========================================");

  dht.begin();
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  xTaskCreatePinnedToCore(cloudTask, "nube", 16384, nullptr, 1, nullptr, 0);
}

void loop()
{
  static uint32_t tSoil = 0, tDht = 0;
  static bool firstDht = true;
  uint32_t now = millis();

  if (now - tSoil >= SOIL_EVERY_MS)
  {
    tSoil = now;
    for (int c = 0; c < NUM_CANALES; c++)
      soilStep(c);
  }
  if (now - tDht >= 3000)
  {
    tDht = now;
    dhtStep();
    if (firstDht || haveDht)
      Serial.printf("[ambiente] %.1f°C %.0f%%%s\n", tempC, humPct, dhtOk ? "" : " (DHT sin lectura)");
    firstDht = false;
  }

  LOCK();
  for (int c = 0; c < NUM_CANALES; c++)
  {
    Canal &k = ch[c];
    if (k.pendingCmd)
    { /* orden de la app para esta planta */
      if (k.relayPin < 0)
        pushEvent(c, "off", "Esta planta no tiene bomba conectada al ESP32");
      else if (k.pendingCmd == 1)
        startPump(c, "manual", MANUAL_S);
      else
        stopPump(c, STOP_APP);
      k.cmdAck = k.pendingCmdId;
      k.pendingCmd = 0;
      k.stateDirty = true;
    }
    if (k.pumpOn && (uint32_t)(millis() - k.pumpStartMs) >= k.pumpForMs)
      stopPump(c, STOP_TIEMPO); /* tiempo máximo */
  }
  UNLOCK();

  digitalWrite(PIN_LED, cloudOk ? HIGH : ((millis() / 400) % 2 ? HIGH : LOW));
  delay(20);
}
