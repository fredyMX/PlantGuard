# PlantGuard · ESP32 real (Nivel 2)

Esta guía explica cómo conectar el ESP32 con el sensor de humedad del suelo, el DHT y la bomba, y cómo enlazarlo con la app.

## Cómo funciona

**Lógica de riego** (la misma de nuestro código de riego automático, ahora con WiFi):

| Situación | Qué hace el ESP32 |
|---|---|
| Humedad **menor al umbral** (ej. 40 %) | Enciende la bomba |
| Humedad llega a **umbral + 20 %** (ej. 60 %) | Apaga la bomba |
| Entre esos dos valores | La bomba conserva su estado (no se prende y apaga a cada rato) |
| Bomba encendida más del **tiempo máximo** (ej. 30 s) | Se apaga y entra en **pausa de seguridad** (ej. 1 min); al terminar vuelve a intentar |

El umbral, el tiempo máximo y la pausa se cambian en la app (**Umbrales → Guardar cambios**). El rango de +20 % se cambia en `config.h` (`HUMEDAD_RANGO`).


```
 Sensores ──► ESP32 ──(WiFi, HTTPS)──► Firestore: dispositivos/{ID} ◄──► App web
              │                          estado   ← ESP32 cada 10 s
              └─ decide el riego          cfg      ← app (Umbrales)
                 aunque no haya WiFi      cmd      ← app (interruptor de la bomba)
                                          lecturas ← ESP32 cada 1 min (historial)
                                          eventos  ← ESP32 (riegos y alertas)
```

- **La decisión de regar la toma el ESP32**, no la página. Si se cae el WiFi o se cierra la app, la planta se sigue regando con el último umbral, que queda guardado en la memoria del ESP32.
- La app solo muestra los datos, manda los umbrales y las órdenes de la bomba.
- Cada ESP32 controla **una maceta**: un sensor de suelo, un DHT y una bomba.

## Material

| Pieza | Nota |
|---|---|
| ESP32 DevKit (30 o 38 pines) | `board = esp32dev` en `platformio.ini` |
| Sensor capacitivo de humedad de suelo v1.2 | No uses el resistivo de dos patas: se corroe en semanas |
| DHT22 (o DHT11) | Si es el sensor suelto de 4 patas, lleva una resistencia de 10 kΩ entre DATA y 3V3 |
| Módulo relé de 1 canal a 5 V | Con optoacoplador, de preferencia |
| Mini bomba sumergible de 3-5 V | Con manguera |
| Fuente de 5 V aparte para la bomba | Un cargador USB de celular de 1 A o más sirve |
| Resistencia de 100 kΩ (opcional) | Sirve para detectar si se desconecta el sensor de suelo |

## Conexiones

| Desde | Hacia el ESP32 |
|---|---|
| Sensor de suelo **VCC** | **3V3** |
| Sensor de suelo **GND** | **GND** |
| Sensor de suelo **AOUT** | **GPIO34** (y opcional: 100 kΩ de GPIO34 a GND) |
| DHT **VCC (+)** | **3V3** |
| DHT **DATA (out)** | **GPIO4** |
| DHT **GND (−)** | **GND** |
| Relé **VCC** | **VIN** (5 V del USB) |
| Relé **GND** | **GND** |
| Relé **IN** | **D18 (GPIO18)** |

Bomba (circuito aparte, a través de los contactos del relé):

```
Fuente 5 V (+) ──► COM del relé
NO del relé    ──► cable (+) rojo de la bomba
Bomba (−) negro ──► Fuente 5 V (−)
```

Usa **NO** (normalmente abierto) para que la bomba quede apagada si el ESP32 se apaga o reinicia.

**Importante:**
- **No alimentes la bomba desde el pin 3V3 ni desde el VIN del ESP32.** El arranque del motor provoca caídas de voltaje que reinician el ESP32 o lo dañan.
- El sensor de suelo debe ir a un pin **ADC1 (GPIO32-39)**. Los pines ADC2 dejan de leer cuando el WiFi está encendido.
- Evita usar los pines 0, 2, 12 y 15 para el relé: el ESP32 los usa al arrancar.
- Si tu sensor de suelo no cambia de valor con 3.3 V, probablemente trae el chip NE555 (necesita 5 V). Aliméntalo desde VIN y revisa con multímetro que AOUT nunca pase de 3.3 V.

## Estructura (proyecto PlatformIO)

```
firmware/
├── platformio.ini          placa, velocidad del monitor y librerías
├── src/
│   └── main.cpp            el programa del ESP32
└── include/
    ├── config.example.h    plantilla (sí se sube a GitHub)
    └── config.h            TU WiFi (no se sube: está en .gitignore)
```

## Preparar VS Code con PlatformIO

1. Instala la extensión **PlatformIO IDE** en VS Code y espera a que termine de instalarse (la primera vez tarda varios minutos).
2. Abre la carpeta del firmware como proyecto: en **PlatformIO Home → Open Project** (o **Archivo → Abrir carpeta**), elige **`PlantGuard/firmware`**.
   Tiene que ser esa carpeta, la que contiene `platformio.ini`. Si abres `PlantGuard` o una carpeta superior, PlatformIO no reconoce el proyecto y los `#include` salen en rojo.
3. La primera vez que compiles, PlatformIO descarga solo la plataforma del ESP32 y las librerías de `platformio.ini` (DHT de Adafruit y ArduinoJson 7). No hay que instalar nada a mano.
4. Si Windows no reconoce la placa, instala el driver **CP210x** o **CH340**, según el chip USB que traiga.

Para trabajar en la app web y en el firmware a la vez, deja abierta la carpeta `PlantGuard` y agrega `firmware` con **Archivo → Agregar carpeta al área de trabajo**.

## Configurar y subir

1. Copia `include/config.example.h` como **`include/config.h`** y escribe tu WiFi. Tiene que ser una red de **2.4 GHz**. Si es una red con portal de acceso (como algunas redes escolares), no va a funcionar; usa el hotspot de un celular.
2. Revisa en `config.h` si tu DHT es `DHT22` o `DHT11`.
3. Conecta el ESP32 por USB. En la barra azul de abajo de VS Code:
   - **✓ (Build)** compila.
   - **→ (Upload)** sube el programa. Si se queda en `Connecting....`, mantén presionado el botón **BOOT** de la placa hasta que empiece a subir.
   - **🔌 (Serial Monitor)** abre el Monitor Serie a 115200 baudios, ya configurado en `platformio.ini`.
4. En el Monitor Serie verás:
   ```
    ID del dispositivo: pg-a1b2c3
   suelo raw=2450 ->  27.3% | 23.1°C 55% | bomba apagada
   ```
   Anota el **ID**.

`config.h` está en `.gitignore` para que la contraseña del WiFi no se suba a GitHub. Cada integrante del equipo tiene que crear su propio `config.h`.

Si tu placa es **ESP32-S3**, cambia `board = esp32dev` por `board = esp32-s3-devkitc-1` en `platformio.ini`. En el S3, el sensor de suelo debe ir a un pin del 1 al 10 (por ejemplo, GPIO4), y el DHT y el relé a otros pines. Cámbialos en `config.h`.

## Calibrar el sensor de suelo

Cada sensor lee distinto, así que hay que calibrarlo una vez:

1. Con el sensor **seco, al aire**, anota el valor `raw`. Esto va en `SOIL_RAW_DRY`.
2. Mete el sensor **en un vaso de agua**, sin pasar la línea blanca, y anota `raw`. Esto va en `SOIL_RAW_WET`.
3. Escribe los dos valores en `config.h` y vuelve a subir el programa.

Cuanta más agua hay, más **bajo** es el valor raw. Valores típicos del capacitivo: seco ≈ 2800-3100, en agua ≈ 1100-1400.

**Si usan el FC-28 / YL-69 (resistivo, con módulo azul):** conecten la salida **AO** (no DO) a D34 y aliméntenlo con **3V3**. Al aire marca ≈ 4095 y en agua ≈ 1000-1700. En `config.h` usen el bloque "Sensor RESISTIVO" (con `SOIL_RAW_MAX_VALID 4096`). Este sensor se corroe con el tiempo por estar siempre energizado; para la demostración funciona bien, pero el capacitivo dura mucho más.

Si `raw` sale menor que `SOIL_RAW_MIN_VALID` o mayor que `SOIL_RAW_MAX_VALID`, el ESP32 da el sensor por desconectado y no riega. La resistencia de 100 kΩ a GND hace que un sensor desconectado lea ~0. Sin ella, el pin queda "al aire" y puede dar valores al azar.

## Preparar Firebase (una sola vez)

1. En **Authentication → Método de acceso**, deja activado **Anónimo**. La app ya lo usa.
2. En **Firestore Database → Reglas**, pega el contenido de `firestore.rules` (en la raíz del proyecto) y presiona **Publicar**. Sin estas reglas, el ESP32 recibe el error `403 PERMISSION_DENIED`.

## Enlazar con la app

1. Abre la app y espera a que diga **☁ Nube** arriba.
2. Ve a **Mi planta → Editar información**, escribe el ID en **ID del ESP32** (por ejemplo `pg-a1b2c3`) y guarda.
3. En **Inicio**, debajo de la humedad, aparece `📡 ESP32 «pg-a1b2c3» · en línea`.
4. En **Umbrales**, presiona **Guardar cambios** para mandarle tus umbrales al ESP32. El Monitor Serie muestra `[nube] umbrales nuevos…`.
5. El interruptor de la bomba en Inicio manda la orden al ESP32. Riega `MANUAL_S` segundos (30 por defecto) y se apaga solo.

Para volver a la simulación, borra el ID de la planta.

El **modo demo** también aplica al ESP32: con el modo demo encendido, "30 min" de espera se mandan como 30 s.

## Servidor local (opcional)

Si ya tienen una página con un servidor en su computadora (por ejemplo, Node en el puerto 3000), el ESP32 puede mandarle cada lectura **además** de Firebase. En `config.h`:

```cpp
#define LOCAL_SERVER_URL "http://192.168.3.80:3000/datos-sensor"   // la IPv4 de tu computadora
#define LOCAL_EVERY_MS   3000UL                                    // cada 3 s
```

Manda un POST con este JSON:

```json
{"humedad":2500,"humedadPct":38.5,"soilOk":true,"temp":23.1,"hum":55,"bomba":false,"id":"pg-a1b2c3"}
```

`humedad` es el valor **crudo** del sensor (0-4095), igual que en el código anterior del equipo, así que un servidor que ya lee `req.body.humedad` sigue funcionando sin cambios. Los demás campos son nuevos y se pueden ignorar.

- La computadora y el ESP32 deben estar en la **misma red WiFi**.
- Si la IP de la computadora cambia, hay que actualizarla en `config.h`.
- Si el Firewall de Windows bloquea el puerto 3000, el Monitor Serie muestra `[local] ... -> -1`.
- Con `LOCAL_SERVER_URL ""` queda desactivado.

## LED integrado

- **Fijo:** conectado a la nube.
- **Parpadeando:** sin WiFi o sin acceso a Firebase. Mira el Monitor Serie para ver la causa.

## Seguridad y protecciones del firmware

- La bomba **nunca** queda encendida más de `PUMP_MAX_S` (60 s), sea riego automático o manual.
- Si el sensor de suelo falla durante un riego automático, la bomba se apaga. Sin lectura no riega.
- Si la bomba llega al tiempo máximo sin alcanzar la humedad objetivo, se apaga y espera la pausa de seguridad antes de volver a intentar. El riego manual no activa esa pausa.
- Las órdenes de la bomba con más de 60 s de antigüedad se ignoran, para que la bomba no se encienda sola al reiniciar.
- El ESP32 se reconecta al WiFi automáticamente y reutiliza su sesión anónima de Firebase.

**Limitaciones (prototipo escolar):**
- La conexión HTTPS no valida el certificado del servidor (`setInsecure()`).
- Cualquier sesión de la app que conozca el ID del dispositivo puede leerlo y mandarle órdenes. Por eso el ID por defecto incluye parte de la MAC.
- Para un producto real habría que ligar cada ESP32 a la cuenta de su dueño y validar certificados.

## Consumo de la cuota gratuita de Firestore (por ESP32, por día)

| Qué | Cantidad aprox. | Límite gratis |
|---|---|---|
| Estado cada 10 s (más uno extra, como máximo cada 2 s, cuando la humedad cambia 2 % o más) | 8,640 + los cambios | 20,000 escrituras |
| Lecturas al historial cada 1 min | 1,440 escrituras | |
| Revisar órdenes cada 5 s | 17,280 lecturas | 50,000 lecturas |
| App abierta viendo el estado en vivo | ~8,640 lecturas | |

Para varios ESP32, o para dejar la app abierta todo el día, sube `STATE_EVERY_MS` y `POLL_EVERY_MS` en `config.h` (por ejemplo, 30 s y 15 s). Si la humedad "brinca" sola y se acaba la cuota, sube `SOIL_SEND_DELTA` (por ejemplo, a 4).

## Problemas comunes

| Síntoma | Causa probable |
|---|---|
| `Falta include/config.h` al compilar | No copiaste `include/config.example.h` como `include/config.h` |
| Los `#include` salen en rojo en VS Code | No abriste la carpeta `firmware` como proyecto de PlatformIO, o falta compilar una vez (✓) |
| Se queda en `[wifi] reconectando…` | Red de 5 GHz, contraseña mal escrita o red con portal |
| `403 PERMISSION_DENIED` | No publicaste `firestore.rules` |
| `no se pudo iniciar sesión` | No está activado el acceso Anónimo en Authentication |
| La app dice "Esperando datos del ESP32" | El ID no coincide (revisa el Monitor Serie) o el ESP32 no tiene internet |
| La humedad siempre marca 0 % o 100 % | Falta calibrar `SOIL_RAW_DRY` y `SOIL_RAW_WET` |
| La bomba funciona al revés | Cambia `RELAY_ACTIVE_LOW` en `config.h` |
| El ESP32 se reinicia al prender la bomba | La bomba toma corriente del ESP32: usa una fuente aparte |
| `DHT sin respuesta` | DHT11 o DHT22 mal elegido en `config.h`, o falta la resistencia de 10 kΩ |
