/* ============================================================
   PlantGuard · configuración del ESP32
   Archivo PERSONAL con tu WiFi. Va en firmware/include/config.h
   Está en .gitignore: NO lo subas a GitHub ni lo compartas.
   ============================================================ */
#pragma once

/* ---------- Red WiFi (solo 2.4 GHz: el ESP32 no ve redes de 5 GHz) ---------- */
#define WIFI_SSID "CASA_ALEO"
#define WIFI_PASS "Un1DdefXxCbV"

/* ---------- Firebase (los mismos datos que js/firebase-config.js) ---------- */
#define FIREBASE_API_KEY    "AIzaSyDr8YW4EE654dXkHLdpDWI3wsHBT3E80BA"
#define FIREBASE_PROJECT_ID "plantguard-c9c60"

/* ID del dispositivo. Vacío = se genera con la MAC (ej.: pg-a1b2c3) y se muestra
   en el Monitor Serie al encender. Ese ID es el que escribes en la app (Mi planta → Editar). */
#define DEVICE_ID ""

/* ---------- Pines ---------- */
#define PIN_SOIL   34      /* salida AOUT del sensor capacitivo. Usa SOLO pines ADC1 (32-39): ADC2 no funciona con WiFi */
#define PIN_DHT    4       /* DATA del DHT11/DHT22 */
#define DHT_TYPE   DHT22   /* cambia a DHT11 si tienes el azul */
#define PIN_RELAY  18      /* D18: IN del relevador, igual que en tu código de riego */
#define RELAY_ACTIVE_LOW false  /* encender = HIGH, apagar = LOW (igual que tu código que sí funcionaba). Si la bomba funciona al revés, pon true */
#define PIN_LED    2       /* LED integrado: fijo = conectado a la nube, parpadeando = sin conexión */

/* ---------- Calibración del sensor de suelo (ver firmware/README.md) ----------
   Mira el valor "raw" en el Monitor Serie (115200 baudios):
   - SECO:   sensor al aire, limpio y seco
   - MOJADO: sensor metido en un vaso de agua hasta la línea blanca                  */
/* Sensor RESISTIVO FC-28 / YL-69 (horquilla + módulo azul LM393). Usa la salida AO, no DO,
   y aliméntalo con 3V3 (con 5 V la salida AO puede pasar de 3.3 V y dañar el ESP32).
   Al aire marca ~4095 (el máximo), por eso SOIL_RAW_MAX_VALID va en 4096.                   */
#define SOIL_RAW_DRY 4095
#define SOIL_RAW_WET 1500          /* cámbialo por el raw que veas con la horquilla en un vaso de agua */
#define SOIL_RAW_MAX_VALID 4096
/* Por debajo de este valor se considera que el sensor está desconectado
   (con una resistencia de 100 kΩ de GPIO34 a GND, un sensor desconectado lee ~0). */
#define SOIL_RAW_MIN_VALID 150

/* ---------- Riego (los primeros tres se cambian desde la app → Umbrales → Guardar) ----------
   Igual que tu código de riego automático:
   - humedad < DEFAULT_THR                 → se ENCIENDE la bomba        (HUMEDAD_MIN)
   - humedad ≥ DEFAULT_THR + HUMEDAD_RANGO → se APAGA la bomba           (HUMEDAD_MAX)
   - encendida más de DEFAULT_DUR segundos → se APAGA y espera DEFAULT_WAIT_MS (pausa de seguridad)  */
#define DEFAULT_THR      40          /* % de humedad mínima (HUMEDAD_MIN) */
#define DEFAULT_DUR      30          /* segundos máximos de bomba encendida seguida (TIEMPO_MAX_BOMBA) */
#define DEFAULT_WAIT_MS  60000UL     /* pausa de seguridad tras un corte por tiempo: 60 s (PAUSA_SEGURIDAD) */
#define HUMEDAD_RANGO    20          /* la bomba se apaga al llegar a umbral + 20 % (40 → 60 %) */
#define MANUAL_S         30          /* duración de un riego manual desde la app */
#define PUMP_MAX_S       60          /* tope absoluto: la bomba nunca queda encendida más que esto */

/* ---------- Servidor local (opcional) ----------
   Además de Firebase, el ESP32 puede mandar cada lectura por POST a un servidor en tu computadora
   (por ejemplo, Node en el puerto 3000). Usa la IPv4 de tu computadora (ipconfig en Windows).
   Formato: {"humedad":2500,"humedadPct":38.5,"soilOk":true,"temp":23.1,"hum":55,"bomba":false,"id":"pg-a1b2c3"}
   "humedad" es el valor CRUDO del sensor, igual que en el código anterior.
   Vacío "" = desactivado.                                                                          */
#define LOCAL_SERVER_URL ""   /* apagado. Para usar tu servidor local: "http://192.168.3.80:3000/datos-sensor" */
#define LOCAL_EVERY_MS   3000UL

/* ---------- Envío a la nube (cuida la cuota gratuita de Firestore) ---------- */
#define STATE_EVERY_MS    10000UL    /* estado en vivo si nada cambia (lo que ve Inicio) */
#define SOIL_EVERY_MS      1000UL    /* lectura del sensor de suelo */
#define SOIL_SEND_DELTA    2.0f      /* si la humedad cambia este % o más, se envía de inmediato... */
#define STATE_MIN_MS       2000UL    /* ...pero nunca más de un envío cada 2 s */
#define POLL_EVERY_MS      5000UL    /* revisar órdenes y umbrales nuevos */
#define READING_EVERY_MS  60000UL    /* lectura guardada en el historial (1 por minuto) */
