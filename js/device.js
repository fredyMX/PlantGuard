/* Conexión con el ESP32 real (Nivel 2) a través de Firestore.
   Documento dispositivos/{ID}:
     estado  → lo escribe el ESP32 cada 10 s (humedad, temperatura, bomba, umbrales aplicados…)
     cfg     → lo escribe la app al guardar Umbrales: { thr, dur, waitMs }
     cmd     → lo escribe la app al usar el interruptor de la bomba: { id, accion: 'on' | 'off' }
   Subcolecciones (las escribe el ESP32): lecturas (cada 5 min) y eventos (riegos, alertas).
   Una planta usa el ESP32 cuando en "Mi planta → Editar" tiene escrito su ID (campo p.device). */
(function () {
  var subs = {}, sentCfg = {}, pending = null, timer = null;
  var ACK_MS = 15000;

  function ready() { return PG.fb && PG.cloud && PG.cloud.state === 'on'; }
  function plants() { var d = PG.plantsData && PG.plantsData(); return d ? d.list : []; }
  function plant(id) { return plants().filter(function (p) { return p.id === id; })[0]; }
  function devRef(dev) { return PG.fb.F.doc(PG.fb.db, 'dispositivos', dev); }
  function report(e) {
    var msg = (e && (e.code || e.message)) || 'error';
    console.warn('[ESP32]', msg);
    if (/permission/i.test(msg)) PG.sim.toast('Firebase rechazó el acceso a los datos del ESP32: revisa las reglas (firestore.rules)', 'bad');
  }

  /* Historial guardado por el ESP32 */
  function loadHistory(pid, dev) {
    var F = PG.fb.F, db = PG.fb.db;
    var q = function (sub, n) { return F.query(F.collection(db, 'dispositivos', dev, sub), F.orderBy('t', 'desc'), F.limit(n)); };
    var data = function (s) { return s.docs.map(function (d) { return d.data(); }).reverse(); };
    return Promise.all([F.getDocs(q('lecturas', 600)), F.getDocs(q('eventos', 80))]).then(function (r) {
      var s = subs[pid]; if (!s || s.dev !== dev) return;
      PG.sim.setHistory(pid, data(r[0]), data(r[1]));
      s.loaded = true;
      if (s.lastEstado) PG.sim.external(pid, s.lastEstado);
    });
  }

  function subscribe(pid, dev) {
    var F = PG.fb.F, db = PG.fb.db, s = { dev: dev, loaded: false, quiet: true };
    subs[pid] = s;
    PG.sim.attach(pid, dev);
    var u1 = F.onSnapshot(devRef(dev), function (snap) {
      var d = snap.exists() ? snap.data() : {};
      if (!d.estado) return;
      s.lastEstado = d.estado;
      if (s.loaded) PG.sim.external(pid, d.estado); /* antes de cargar el historial se guarda y se aplica después */
      if (pending && pending.pid === pid && d.estado.cmdAck === pending.id) done(true);
    }, report);
    var u2 = F.onSnapshot(F.query(F.collection(db, 'dispositivos', dev, 'eventos'), F.orderBy('t', 'desc'), F.limit(20)), function (snap) {
      if (!s.loaded) return; /* la carga inicial ya trae los eventos */
      PG.sim.deviceEvents(pid, snap.docs.map(function (d) { return d.data(); }).reverse(), s.quiet);
      s.quiet = false;
    }, report);
    s.off = function () { u1(); u2(); };
    loadHistory(pid, dev).then(function () {
      /* los eventos que llegaron mientras se cargaba ya están en el historial; de aquí en adelante sí se avisan */
      s.quiet = false;
    }).catch(report);
  }

  /* Umbrales de cada planta → cfg del ESP32 (solo si cambiaron) */
  function pushCfg() {
    if (!ready()) return;
    plants().forEach(function (p) {
      if (!p.device || !p.cfg) return;
      var c = { thr: +p.cfg.thr, dur: +p.cfg.dur, waitMs: Math.round(p.cfg.wait * (PG.state.demo ? 1000 : 60000)) };
      var k = p.device + JSON.stringify(c);
      if (sentCfg[p.id] === k) return;
      sentCfg[p.id] = k;
      PG.fb.F.setDoc(devRef(p.device), { cfg: c }, { merge: true }).catch(function (e) { delete sentCfg[p.id]; report(e); });
    });
  }

  function sync() {
    if (!ready()) return;
    var want = {};
    plants().forEach(function (p) { if (p.device) want[p.id] = p.device; });
    Object.keys(subs).forEach(function (pid) {
      if (want[pid] === subs[pid].dev) return;
      subs[pid].off(); delete subs[pid]; delete sentCfg[pid];
      if (!want[pid]) PG.sim.detach(pid);
    });
    Object.keys(want).forEach(function (pid) { if (!subs[pid]) subscribe(pid, want[pid]); });
    pushCfg();
  }
  function later() { clearTimeout(timer); timer = setTimeout(sync, 300); }

  function done(ok) {
    if (!pending) return;
    clearTimeout(pending.timer);
    if (!ok) PG.sim.toast('El ESP32 no respondió a la orden. ¿Está encendido y con WiFi?', 'bad');
    pending = null;
    PG.emit('state', 'device');
  }

  PG.device = {
    /* Interruptor de la bomba en Inicio para una planta con ESP32 */
    toggle: function (pid) {
      var p = plant(pid); if (!p || !p.device) return;
      if (!ready()) { PG.sim.toast('Sin conexión a la nube: no se puede mandar la orden al ESP32', 'bad'); return; }
      var info = PG.sim.device();
      if (info && !info.online) { PG.sim.toast('El ESP32 «' + p.device + '» está sin conexión', 'bad'); return; }
      if (pending) return;
      var id = Date.now(), accion = PG.state.pump ? 'off' : 'on';
      pending = { pid: pid, id: id, accion: accion, timer: setTimeout(function () { done(false); }, ACK_MS) };
      PG.emit('state', 'device');
      PG.fb.F.setDoc(devRef(p.device), { cmd: { id: id, accion: accion } }, { merge: true })
        .catch(function (e) { report(e); done(false); });
    },
    pending: function (pid) { return pending && pending.pid === pid ? pending.accion : ''; }
  };

  PG.on('cloud', function (c) { if (c.state === 'on') sync(); });
  PG.on('plants:changed', later);
  PG.on('plants:reload', later);
})();
