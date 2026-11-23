/* ============================================================
   PlantGuard · configuración del ESP32
   Archivo PERSONAL con tu WiFi. Va en firmware/include/config.h
   Está en .gitignore: NO lo subas a GitHub ni lo compartas.
   ============================================================ */
#pragma once

/* ---------- Red WiFi (solo 2.4 GHz: el ESP32 no ve redes de 5 GHz) ---------- */
#define WIFI_SSID "realme 14 Pro 5G C954"
#define WIFI_PASS "pokefredy"

/* ---------- Firebase (los mismos datos que js/firebase-config.js) ---------- */
#define FIREBASE_API_KEY "AIzaSyDr8YW4EE654dXkHLdpDWI3wsHBT3E80BA"
#define FIREBASE_PROJECT_ID "plantguard-c9c60"

/* ID del dispositivo. Vacío = se genera con la MAC (ej.: pg-a1b2c3) y se muestra
   en el Monitor Serie al encender. Ese ID es el que escribes en la app (Mi planta → Editar). */
#define DEVICE_ID ""

/* ---------- Pines compartidos ---------- */
#define PIN_DHT 4      /* DATA del DHT11/DHT22 (uno solo para las dos plantas) */
#define DHT_TYPE DHT22 /* cambia a DHT11 si tienes el azul */
#define PIN_LED 2      /* LED integrado: fijo = conectado a la nube, parpadeando = sin conexión */

/* ---------- Plantas (canales) ----------
   Cada canal = un sensor de suelo + una bomba = una planta en la app.
   - Planta 1 usa el ID que sale en el Monitor Serie (ej. pg-205cdc)
   - Planta 2 usa el mismo ID con "-2" al final (ej. pg-205cdc-2)
   Sensores: SOLO pines ADC1 (GPIO32-39), porque ADC2 no funciona con WiFi.
   Calibración: mira el "Lectura" de cada planta en el Monitor Serie
     RAW_DRY = sensor al aire · RAW_WET = sensor en un vaso de agua.
   Resistivos (FC-28, MH-sensor-series, HW-080): usa la salida AO (no DO) y aliméntalos con 3V3.   */
#define NUM_CANALES 2      /* 1 = una planta, 2 = dos plantas */
#define BOMBA_COMPARTIDA 1 /* 1 = UNA sola bomba (la del relevador de la planta 1) riega las dos plantas: \
                              se enciende si alguna pide agua y se apaga cuando ninguna pide.             \
                              0 = cada planta con su propia bomba (CH2_RELAY_PIN).          */

/* Planta 1 · MH-sensor-series (resistivo, módulo LM393) */
#define CH1_SOIL_PIN 34            /* D34 */
#define CH1_RELAY_PIN 18           /* D18: IN del relevador (la única bomba) */
#define CH1_RELAY_ACTIVE_LOW false /* HIGH = bomba encendida: comprobado con el Monitor Serie (con true, pin en HIGH la bomba seguía prendida) */
#define CH1_RAW_DRY 4095
#define CH1_RAW_WET 1500
#define CH1_RAW_MIN_VALID 150 /* menos que esto = sensor desconectado */
#define CH1_RAW_MAX_VALID 4096

/* Planta 2 · HW-080 (resistivo, módulo LM393) */
#define CH2_SOIL_PIN 35                           /* D35, junto a D34 */
#define CH2_RELAY_PIN -1                          /* no se usa: con BOMBA_COMPARTIDA 1 la planta 2 usa la bomba de D18 */
#define CH2_RELAY_ACTIVE_LOW CH1_RELAY_ACTIVE_LOW /* igual que el 1; cámbialo si el relevador 2 funciona al revés */
#define CH2_RAW_DRY 4095
#define CH2_RAW_WET 1500 /* cámbialo por lo que marque el HW-080 en un vaso de agua */
#define CH2_RAW_MIN_VALID 150
#define CH2_RAW_MAX_VALID 4096

/* Nombres de versiones anteriores (por si alguien compila con un main.cpp viejo: usa la planta 1) */
#define PIN_SOIL CH1_SOIL_PIN
#define PIN_RELAY CH1_RELAY_PIN
#define RELAY_ACTIVE_LOW CH1_RELAY_ACTIVE_LOW
#define SOIL_RAW_DRY CH1_RAW_DRY
#define SOIL_RAW_WET CH1_RAW_WET
#define SOIL_RAW_MIN_VALID CH1_RAW_MIN_VALID
#define SOIL_RAW_MAX_VALID CH1_RAW_MAX_VALID

/* ---------- Riego (valores iniciales; cada planta cambia los suyos en la app → Umbrales → Guardar) ----------
   Igual que tu código de riego automático:
   - humedad < DEFAULT_THR                 → se ENCIENDE la bomba        (HUMEDAD_MIN)
   - humedad ≥ DEFAULT_THR + HUMEDAD_RANGO → se APAGA la bomba           (HUMEDAD_MAX)
   - encendida más de DEFAULT_DUR segundos → se APAGA y espera DEFAULT_WAIT_MS (pausa de seguridad)  */
#define DEFAULT_THR 40          /* % de humedad mínima (HUMEDAD_MIN) */
#define DEFAULT_DUR 30          /* segundos máximos de bomba encendida seguida (TIEMPO_MAX_BOMBA) */
#define DEFAULT_WAIT_MS 60000UL /* pausa de seguridad tras un corte por tiempo: 60 s (PAUSA_SEGURIDAD) */
#define HUMEDAD_RANGO 20        /* la bomba se apaga al llegar a umbral + 20 % (40 → 60 %) */
#define MANUAL_S 30             /* duración de un riego manual desde la app */
#define PUMP_MAX_S 60           /* tope absoluto: la bomba nunca queda encendida más que esto */

/* ---------- Servidor local (opcional) ----------
   Además de Firebase, el ESP32 puede mandar cada lectura por POST a un servidor en tu computadora
   (por ejemplo, Node en el puerto 3000). Usa la IPv4 de tu computadora (ipconfig en Windows).
   Formato: {"humedad":2500,"humedadPct":38.5,"soilOk":true,"temp":23.1,"hum":55,"bomba":false,"id":"pg-a1b2c3"}
   "humedad" es el valor CRUDO del sensor, igual que en el código anterior.
   Vacío "" = desactivado.                                                                          */
#define LOCAL_SERVER_URL "" /* apagado. Para usar tu servidor local: "http://192.168.3.80:3000/datos-sensor" */
#define LOCAL_EVERY_MS 3000UL

/* ---------- Envío a la nube (cuida la cuota gratuita de Firestore) ----------
   Con 2 plantas todo se envía el doble; estos valores dejan margen dentro del plan gratis.   */
#define STATE_EVERY_MS 20000UL   /* estado en vivo si nada cambia (lo que ve Inicio) */
#define SOIL_EVERY_MS 1000UL     /* lectura de los sensores de suelo */
#define SOIL_SEND_DELTA 3.0f     /* si la humedad cambia este % o más, se envía de inmediato... */
#define STATE_MIN_MS 5000UL      /* ...pero nunca más de un envío cada 5 s por planta */
#define POLL_EVERY_MS 10000UL    /* revisar órdenes y umbrales nuevos */
#define READING_EVERY_MS 60000UL /* lectura guardada en el historial (1 por minuto) */
