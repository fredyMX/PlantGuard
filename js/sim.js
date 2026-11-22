/* Motor de simulación (Nivel 1): una simulación INDEPENDIENTE por planta.
   Cada planta tiene sus propios sensores (humedad del suelo, temperatura, humedad ambiente), bomba, espera entre riegos,
   historial y alertas, y todas siguen funcionando aunque no sea la planta activa. PG.state refleja la planta activa.
   Todo corre en el navegador; los historiales se guardan en localStorage (y en Firebase si está configurado).
   Nivel 2: si una planta tiene un ESP32 vinculado (p.device), NO se simula: sus datos llegan de js/device.js
   con PG.sim.external() y PG.sim.deviceEvents(). */
(function () {
  var S = PG.state, KEY = 'pg_hist_v2', OLD = 'pg_hist_v1', SET = 'pg_ajustes_v1';
  var R = {}, cur = null, stepId = null, H = { readings: [], events: [] }, off = 0, flag = {}, n = 0, store = {};
  var RF = ['soil', 'temp', 'hum', 'ok', 'pump', 'pumpMode', 'pumpLeft', 'lastEnd', 'startAt'];
  var now = function () { return Date.now(); };
  var rnd = function (a, b) { return a + Math.random() * (b - a); };
  var clamp = function (x, a, b) { return Math.max(a, Math.min(b, x)); };
  var r1 = function (x) { return Math.round(x * 10) / 10; };
  var TZ = 'America/Mexico_City';

  Object.assign(S, { soil: 28, temp: 23, hum: 61, ok: true, pump: false, pumpMode: '', pumpLeft: 0, lastEnd: 0, startAt: 0, demo: true });
  try { Object.assign(S, JSON.parse(localStorage.getItem(SET)) || {}); } catch (e) { }
  try { store = JSON.parse(localStorage.getItem(KEY)) || {}; } catch (e) { store = {}; }
  if (!Object.keys(store).length) { try { var o = JSON.parse(localStorage.getItem(OLD)); if (o && o.readings && o.readings.length) store.p1 = o; } catch (e) { } }

  /* ---------- Plantas y su "clima" propio ---------- */
  function plantList() {
    var d = PG.plantsData && PG.plantsData();
    return d && d.list && d.list.length ? d.list : [{ id: 'p1', nombre: 'La Dormilona', tipo: 'Interior', icono: 'mimosa' }];
  }
  function byId(id) { return plantList().filter(function (p) { return p.id === id; })[0]; }
  var BASE = { Interior: { temp: 23, hum: 58, amp: 1 }, Exterior: { temp: 25, hum: 48, amp: 4 }, Invernadero: { temp: 27, hum: 75, amp: 2 } };
  var DRY = { cactus: .4, suculenta: .5, bonsai: .8, mimosa: 1, monstera: 1, flor: 1.2, hierbas: 1.3, helecho: 1.5 };
  function prof(p) { /* temperatura/humedad típicas según el tipo, velocidad de secado según el ícono, y un desfase único por planta */
    var b = BASE[p && p.tipo] || BASE.Interior, h = 0, s = (p && p.id) || 'p1', i;
    for (i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) % 1000;
    var u = h / 1000 - 0.5;
    return { temp: b.temp + u * 3, hum: b.hum + u * 12, amp: b.amp, dry: DRY[p && p.icono] || 1 };
  }
  function hourMX() {
    var h = parseInt(new Date(now()).toLocaleString('en-GB', { hour: '2-digit', hour12: false, timeZone: TZ }), 10);
    return isNaN(h) ? 12 : h % 24;
  }

  function level(k, v, thr) {
    var t = thr == null ? S.thr : thr;
    if (k === 'soil') return v < t - 10 ? 'bad' : v < t ? 'warn' : 'ok';
    if (k === 'temp') return v >= 18 && v <= 28 ? 'ok' : v >= 15 && v <= 32 ? 'warn' : 'bad';
    return v >= 40 && v <= 70 ? 'ok' : v >= 30 && v <= 80 ? 'warn' : 'bad';
  }
  function hm(d) { return d.toLocaleTimeString('es-MX', { hour: '2-digit', minute: '2-digit', hour12: false, timeZone: TZ }); }
  function dayKey(d) { return d.toLocaleDateString('es-MX', { timeZone: TZ }); }
  function fmtWhen(t) {
    var d = new Date(t);
    return dayKey(d) === dayKey(new Date()) ? 'Hoy, ' + hm(d)
      : d.toLocaleDateString('es-MX', { day: '2-digit', month: '2-digit', timeZone: TZ }) + ' ' + hm(d);
  }
  function waitMs() { return S.wait * (S.demo ? 1000 : 60000); }

  /* ---------- Historial ---------- */
  function seed(r, pr) {
    var t0 = now(), d = { readings: [], events: [] }, soil = 55, t, i;
    /* Semana anterior: una lectura cada 3 h, sin pasar de 10 min antes de ahora */
    for (t = t0 - 7 * 864e5; t < t0 - 6e5; t += 3 * 36e5) {
      soil = clamp(soil + rnd(-9, 6), 25, 75);
      d.readings.push({ t: t, soil: r1(soil), temp: r1(pr.temp + rnd(-2, 3)), hum: Math.round(pr.hum + rnd(-8, 8)) });
      if (soil < 38) { d.events.push({ t: t, type: 'auto', msg: 'Riego automático activado · humedad al ' + Math.round(soil) + '%' }); soil += 18; }
    }
    /* Últimas 40 lecturas (2 min), una cada 3 s y terminando en este instante */
    for (i = 40; i >= 1; i--) {
      d.readings.push({ t: t0 - i * 3000, soil: r1(clamp(r.soil + i * 0.15, 5, 95)), temp: r1(r.temp + rnd(-0.3, 0.3)), hum: Math.round(r.hum + rnd(-1, 1)) });
    }
    return d;
  }
  function save() {
    if (cur && R[cur]) R[cur].H = H;
    var ids = plantList().map(function (p) { return p.id; });
    ids.forEach(function (id) { if (R[id]) store[id] = R[id].H; });
    Object.keys(store).forEach(function (id) { if (ids.indexOf(id) < 0) delete store[id]; });
    try { localStorage.setItem(KEY, JSON.stringify(store)); } catch (e) { }
  }

  /* ---------- Estado de cada planta ---------- */
  function ensure(p) {
    if (R[p.id]) return R[p.id];
    var pr = prof(p), h = store[p.id], has = h && h.readings && h.readings.length, last = has ? h.readings[h.readings.length - 1] : null;
    var r = {
      soil: last ? last.soil : (p.id === 'p1' ? 28 : r1(rnd(45, 65))),
      temp: last ? last.temp : r1(pr.temp + rnd(-1, 1)),
      hum: last ? last.hum : Math.round(pr.hum + rnd(-3, 3)),
      ok: true, pump: false, pumpMode: '', pumpLeft: 0, lastEnd: 0, startAt: 0, off: 0, flag: {}
    };
    if (p.device) { r.ok = false; r.dev = { id: p.device, t: 0, wait: true }; }
    r.H = has ? h : p.device ? { readings: [], events: [] } : seed(r, pr);
    R[p.id] = r; return r;
  }
  function load(id) { var r = R[id]; RF.forEach(function (k) { S[k] = r[k]; }); H = r.H; off = r.off; flag = r.flag; }
  function keep(id) { var r = R[id]; RF.forEach(function (k) { r[k] = S[k]; }); r.H = H; r.off = off; r.flag = flag; }
  function select(id) {
    var p = byId(id) || plantList()[0];
    if (cur && R[cur]) keep(cur);
    ensure(p); cur = stepId = p.id; load(p.id);
  }
  function syncCur() {
    var list = plantList();
    Object.keys(R).forEach(function (id) { if (!list.some(function (p) { return p.id === id; })) delete R[id]; });
    if (!cur || !R[cur]) select(PG.plant && byId(PG.plant.id) ? PG.plant.id : list[0].id);
  }
  var live = function () { return stepId === cur; };
  var emitState = function (x) { if (live()) PG.emit('state', x); };
  var emitHist = function () { if (live()) PG.emit('hist'); };
  var plantName = function (id) { return (byId(id) || {}).nombre || 'Planta'; };

  function toast(msg, kind) {
    var c = document.getElementById('toasts'); if (!c) return;
    var d = document.createElement('div'); d.className = 'toast ' + (kind || 'info'); d.textContent = msg;
    c.appendChild(d); setTimeout(function () { d.remove(); }, 4500);
  }
  function log(type, msg, kind) {
    var ev = { t: now(), type: type, msg: msg };
    H.events.push(ev);
    if (H.events.length > 80) H.events.shift();
    if (kind) toast((live() ? '' : plantName(stepId) + ': ') + msg, kind);
    PG.emit('event', { plant: stepId, t: ev.t, type: type, msg: msg });
    emitHist();
  }

  function startPump(mode) {
    if (S.pump) return;
    S.pump = true; S.pumpMode = mode; S.pumpLeft = mode === 'auto' ? S.dur : 30; S.startAt = now();
    log(mode, (mode === 'auto' ? 'Riego automático activado' : 'Riego manual iniciado') + ' · humedad al ' + Math.round(S.soil) + '%', mode === 'auto' ? 'info' : '');
    emitState('pump');
  }
  function stopPump() {
    if (!S.pump) return;
    var sec = Math.round((now() - S.startAt) / 1000);
    S.pump = false; S.pumpLeft = 0;
    if (S.pumpMode === 'auto') S.lastEnd = now(); /* el riego manual no frena al automático (HU05) */
    log('stop', 'Riego terminado · duró ' + sec + ' s (humedad al ' + Math.round(S.soil) + '%)');
    emitState('pump');
  }
  function fail(k) {
    if (!S.ok) return;
    S.ok = false; off = k;
    log('off', 'Sensor de suelo desconectado', 'bad');
    emitState('ok');
  }

  /* Una lectura de los sensores de la planta que está cargada en S */
  function read() {
    if (off > 0 && --off === 0) { S.ok = true; log('on', 'Sensor de suelo reconectado', 'ok'); }
    else if (off === 0 && Math.random() < 0.012) return fail(4);
    var pr = prof(byId(stepId)), wave = Math.sin((hourMX() - 9) / 24 * 2 * Math.PI); /* máximo de calor a las 15:00 */
    if (S.ok) S.soil = clamp(S.soil + (S.pump ? rnd(3, 5) : -rnd(0.3, 1.1) * pr.dry), 5, 95);
    S.temp = clamp(S.temp + (pr.temp + pr.amp * wave - S.temp) * 0.1 + rnd(-0.4, 0.4), 10, 40);
    S.hum = clamp(S.hum + (pr.hum - pr.amp * 1.5 * wave - S.hum) * 0.1 + rnd(-1.5, 1.5), 20, 95);
    if (!S.ok) return;
    var bad = level('soil', S.soil) === 'bad';
    if (bad && !flag.crit) log('crit', 'Humedad crítica detectada · ' + Math.round(S.soil) + '% en el suelo', 'bad');
    flag.crit = bad;
    if (!S.pump && S.soil < S.thr && now() - S.lastEnd >= waitMs()) startPump('auto');
    var rec = { t: now(), soil: r1(S.soil), temp: r1(S.temp), hum: Math.round(S.hum) };
    H.readings.push(rec);
    if (H.readings.length > 600) H.readings.shift();
    emitHist();
    PG.emit('reading', { plant: stepId, t: rec.t, soil: rec.soil, temp: rec.temp, hum: rec.hum });
  }
  function step() {
    if (S.pump && --S.pumpLeft <= 0) stopPump();
    if (n % 3 === 0) read();
  }
  function tick() {
    n++; syncCur();
    var act = cur, mine = { thr: S.thr, dur: S.dur, wait: S.wait };
    keep(act);
    plantList().forEach(function (p) { /* las demás plantas siguen su propia simulación con sus propios umbrales */
      if (p.id === act) return;
      if (p.device) { devTick(p, ensure(p)); return; } /* ESP32 real: no se simula */
      ensure(p); stepId = p.id; load(p.id);
      var c = p.cfg || { thr: 35, dur: 10, wait: 30 }; S.thr = c.thr; S.dur = c.dur; S.wait = c.wait;
      step(); keep(p.id);
    });
    stepId = act; load(act); S.thr = mine.thr; S.dur = mine.dur; S.wait = mine.wait;
    var ap = byId(act);
    if (ap && ap.device) devTick(ap, R[act], S); else step();
    if (n % 15 === 0) save();
    PG.emit('state', 'tick');
  }

  /* ---------- ESP32 real ---------- */
  var ONLINE_MS = 35000; /* el ESP32 manda su estado cada 10 s; si pasan 35 s sin nada, se considera sin conexión */
  function devOf(p, r) {
    if (!r.dev || r.dev.id !== p.device) r.dev = { id: p.device, t: 0, wait: true };
    return r.dev;
  }
  function devTick(p, r, o) { /* o: dónde viven los valores (S si es la planta activa, si no R[id]) */
    o = o || r; var d = devOf(p, r);
    d.online = !d.wait && now() - d.t < ONLINE_MS;
    if (o.pump && o.pumpLeft > 0) o.pumpLeft--; /* cuenta regresiva local entre un reporte y otro */
  }
  function histOf(id) { return id === cur ? H : R[id].H; }

  function liveData() {
    var last = null, i;
    for (i = H.events.length - 1; i >= 0; i--) if (H.events[i].type === 'auto' || H.events[i].type === 'manual') { last = H.events[i]; break; }
    var rem = S.lastEnd + waitMs() - now(), l = level('soil', S.soil);
    return {
      apagadoAutomaticoAl: devInfo() && devInfo().max ? devInfo().max + '% de humedad (ESP32)' : undefined,
      pausaDeSeguridad: devInfo() ? !!devInfo().pausa : undefined,
      fuenteDatos: devInfo() ? 'ESP32 real «' + devInfo().id + '»' + (devInfo().online ? '' : ' (sin conexión)') : 'Simulación en el navegador',
      nombrePlanta: (PG.plant ? PG.plant.nombre + (PG.plant.especie ? ' (' + PG.plant.especie + ')' : '') : 'La Dormilona (Mimosa pudica)'),
      ubicacion: PG.plant ? PG.plant.tipo + (PG.plant.ubicacion ? ', ' + PG.plant.ubicacion : '') : 'Interior, Sala junto a la ventana',
      humedadSuelo: S.ok ? Math.round(S.soil) + '%' : 'sin lectura (sensor desconectado)',
      umbralMinimo: S.thr + '%',
      estadoHumedad: !S.ok ? 'Sin lectura' : l === 'ok' ? 'Dentro del rango' : l === 'warn' ? 'Por debajo del umbral' : 'Crítica',
      temperatura: r1(S.temp) + '°C', humedadAmb: Math.round(S.hum) + '%', presion: '1013 hPa', luz: '540 lx',
      estadoBomba: S.pump ? 'Activa · riega ' + S.pumpLeft + ' s más' : 'Apagada',
      duracionRiegoConfigurada: S.dur + ' s',
      esperaMinimaEntreRiegos: S.wait + (S.demo ? ' s (modo demo)' : ' min'),
      ultimoRiego: last ? fmtWhen(last.t) : 'Sin riegos registrados',
      proximoEstimado: S.pump ? 'Regando ahora' : !S.ok ? 'No disponible mientras el sensor esté desconectado'
        : S.soil >= S.thr ? 'Se regará cuando la humedad baje de ' + S.thr + '%'
          : rem > 0 ? 'En espera ' + Math.ceil(rem / 1000) + ' s antes de volver a regar' : 'En la próxima lectura'
    };
  }

  function devInfo() { var p = byId(cur); return p && p.device && R[cur] ? devOf(p, R[cur]) : null; }

  PG.sim = {
    level: level, fmtWhen: fmtWhen, toast: toast, TZ: TZ, liveData: liveData, _tick: tick, select: select,
    waitLeft: function () {
      var d = devInfo(), ms = d && d.cfg && d.cfg.waitMs ? d.cfg.waitMs : waitMs(); /* con ESP32: la espera que él tiene aplicada */
      return S.lastEnd ? Math.max(0, Math.ceil((S.lastEnd + ms - now()) / 1000)) : 0;
    },
    toggle: function () {
      if (devInfo()) { if (PG.device) PG.device.toggle(cur); return; } /* con ESP32 la orden va a la nube */
      if (S.pump) stopPump(); else startPump('manual');
    },
    fail: function () { if (!devInfo()) fail(4); },
    device: devInfo,
    /* Resumen rápido de todas las plantas (para las pastillas de Inicio) */
    summary: function () {
      return plantList().map(function (p) {
        var a = p.id === cur, r = a ? S : R[p.id];
        if (!r) return { id: p.id, soil: null, ok: true, pump: false, lv: 'ok' };
        var off = p.device && R[p.id] && !(R[p.id].dev && R[p.id].dev.online);
        return { id: p.id, soil: r.soil, ok: r.ok, pump: r.pump, lv: !r.ok || off ? 'bad' : level('soil', r.soil, a ? S.thr : (p.cfg || { thr: 35 }).thr) };
      });
    },
    saveSettings: function () {
      try { localStorage.setItem(SET, JSON.stringify({ thr: S.thr, dur: S.dur, wait: S.wait, demo: S.demo })); } catch (e) { }
      toast('Ajustes guardados' + (PG.plant ? ' para ' + PG.plant.nombre : ''), 'ok');
    },
    clear: function () {
      if (devInfo()) return; /* el historial del ESP32 es real: no se reinicia con datos de ejemplo */
      PG.emit('history:clear', cur);
      H = seed({ soil: S.soil, temp: S.temp, hum: S.hum }, prof(byId(cur))); R[cur].H = H; save(); PG.emit('hist');
    },
    setHistory: function (id, r, e) {
      var p = byId(id); if (!p) return;
      var x = ensure(p), d = (r.length || e.length || p.device) ? { readings: r, events: e } : seed(x, prof(p));
      if (r.length) { var l = r[r.length - 1]; x.soil = l.soil; x.temp = l.temp; x.hum = l.hum; if (id === cur) { S.soil = l.soil; S.temp = l.temp; S.hum = l.hum; } }
      x.H = d; if (id === cur) { H = d; PG.emit('hist'); }
      save();
    },
    /* Vincula una planta a un ESP32: se borra lo simulado y se espera el primer reporte */
    attach: function (id, devId) {
      var p = byId(id); if (!p) return;
      var x = ensure(p), o = id === cur ? S : x;
      x.dev = { id: devId, t: 0, wait: true }; o.ok = false; o.pump = false; o.pumpLeft = 0;
      x.H = { readings: [], events: [] }; if (id === cur) { H = x.H; PG.emit('hist'); PG.emit('state', 'device'); }
    },
    /* Se quitó el ESP32: la planta vuelve a la simulación */
    detach: function (id) {
      var x = R[id]; if (!x) return;
      delete x.dev; var o = id === cur ? S : x; o.ok = true; o.pump = false; o.pumpLeft = 0;
      if (id === cur) { off = 0; x.off = 0; PG.emit('state', 'device'); }
    },
    /* Estado en vivo publicado por el ESP32 en dispositivos/{id}.estado */
    external: function (id, e) {
      var p = byId(id); if (!p || !p.device || !e) return;
      var x = ensure(p), d = devOf(p, x), o = id === cur ? S : x, t = typeof e.t === 'number' ? e.t : 0;
      if (!t || t <= d.t) return; /* nada nuevo */
      d.t = t; d.wait = false; d.online = now() - t < ONLINE_MS;
      d.rssi = e.rssi; d.fw = e.fw; d.ip = e.ip; d.cmdAck = e.cmdAck; d.dhtOk = e.dhtOk !== false; d.raw = e.soilRaw;
      d.cfg = { thr: e.thr, dur: e.dur, waitMs: e.waitMs };
      d.max = e.max; d.pausa = !!e.pausa; /* firmware 1.3: se apaga al llegar a max; pausa de seguridad tras un corte por tiempo */
      if (typeof e.soil === 'number') o.soil = e.soil;
      if (typeof e.temp === 'number') o.temp = e.temp;
      if (typeof e.hum === 'number') o.hum = e.hum;
      o.ok = e.soilOk !== false;
      o.pump = !!e.pump; o.pumpMode = e.pump ? (e.pumpMode || 'auto') : ''; o.pumpLeft = e.pumpLeft || 0;
      o.lastEnd = e.lastEnd || 0;
      if (o.ok) {
        var h = histOf(id);
        h.readings.push({ t: t, soil: r1(o.soil), temp: r1(o.temp), hum: Math.round(o.hum) });
        if (h.readings.length > 600) h.readings.shift();
      }
      if (id === cur) { PG.emit('state', 'device'); PG.emit('hist'); }
      PG.emit('device', id);
    },
    /* Eventos que escribió el ESP32 (riegos, alertas). quiet = no mostrar avisos (carga inicial) */
    deviceEvents: function (id, list, quiet) {
      var p = byId(id); if (!p || !p.device) return;
      ensure(p);
      var h = histOf(id), last = h.events.length ? h.events[h.events.length - 1].t : 0, any = false;
      var KIND = { crit: 'bad', off: 'bad', on: 'ok', auto: 'info', pausa: 'bad' };
      list.forEach(function (e) {
        if (!(e.t > last)) return;
        h.events.push({ t: e.t, type: e.type, msg: e.msg }); any = true;
        if (h.events.length > 80) h.events.shift();
        if (!quiet && KIND[e.type]) toast((id === cur ? '' : p.nombre + ': ') + e.msg, KIND[e.type]);
      });
      if (any && id === cur) PG.emit('hist');
    }
  };
  Object.defineProperty(PG.sim, 'hist', { get: function () { return H; } });
  window.addEventListener('beforeunload', save);
  setInterval(tick, 1000);
})();
