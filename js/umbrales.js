/* Pantalla Umbrales: humedad mínima, duración y espera entre riegos */
(function () {
  var html = [
    '<h1 class="title">Umbrales de riego</h1><p class="sub" id="usub"></p>',
    '<div class="card"><div class="row"><b>Umbral de humedad mínima</b><output id="o1"></output></div>',
    '<input type="range" id="r1" min="10" max="80" aria-label="Umbral de humedad mínima"><div class="scale"><span>10%</span><span>80%</span></div>',
    '<p class="muted" style="margin-top:8px" id="h1">Si la humedad del suelo cae por debajo de este valor, la bomba se activa automáticamente.</p></div>',
    '<div class="card"><div class="row"><b>Duración máxima del riego</b><output id="o2"></output></div>',
    '<input type="range" id="r2" min="5" max="60" aria-label="Duración máxima del riego"><div class="scale"><span>5 s</span><span>60 s</span></div>',
    '<p class="muted" style="margin-top:8px" id="h2">Tiempo que la bomba permanece encendida en cada riego.</p></div>',
    '<div class="card"><div class="row"><b>Espera entre riegos</b><output id="o3"></output></div>',
    '<input type="range" id="r3" min="1" max="120" step="1" aria-label="Espera entre riegos"><div class="scale"><span>1 min</span><span>120 min</span></div>',
    '<p class="muted" style="margin-top:8px" id="h3">Evita ciclos de riego demasiado frecuentes tras una activación.</p></div>',
    '<div class="card"><div class="row"><b>Modo demo</b><input type="checkbox" id="demo" aria-label="Modo demo"></div>',
    '<p class="muted">Acorta la espera entre riegos (1 min = 1 s) para ver el ciclo completo en la presentación.</p>',
    '<p class="muted" id="dcap" style="margin-top:6px;font-weight:600"></p>',
    '<p class="muted" style="margin-top:6px">Para verlo: sube el umbral por encima de la humedad actual y mira la cuenta regresiva en Inicio.</p></div>',
    '<button class="save" id="save">Guardar cambios</button>'
  ].join('');

  PG.register('umbrales', {
    html: html,
    init: function (sec) {
      var map = [['r1', 'o1', 'thr', '%'], ['r2', 'o2', 'dur', ' s'], ['r3', 'o3', 'wait', ' min']];
      map.forEach(function (m) {
        var r = sec.querySelector('#' + m[0]), o = sec.querySelector('#' + m[1]);
        r.value = PG.state[m[2]]; o.textContent = r.value + m[3];
        r.oninput = function () { o.textContent = r.value + m[3]; PG.set(m[2], +r.value); };
      });
      var d = sec.querySelector('#demo');
      var o3 = sec.querySelector('#o3'), r3 = sec.querySelector('#r3'), cap = sec.querySelector('#dcap');
      function paint() {
        var w = PG.state.wait, dm = PG.state.demo, dev = PG.sim.device();
        o3.textContent = dm ? w + ' min → ' + w + ' s' : w + ' min';
        cap.textContent = dm ? 'Modo demo ACTIVADO: la espera real ahora es de ' + w + ' s.' : 'Modo demo apagado: la espera real es de ' + w + ' min.';
        /* Con ESP32 la lógica es la del firmware: encender bajo el umbral, apagar en umbral + rango o por tiempo, y pausa */
        var rango = dev && dev.max && dev.cfg && dev.cfg.thr ? dev.max - dev.cfg.thr : 20, max = Math.min(100, PG.state.thr + rango);
        var wtxt = dm ? w + ' s' : w + ' min';
        sec.querySelector('#h1').textContent = dev
          ? 'El ESP32 enciende la bomba bajo ' + PG.state.thr + '% y la apaga al llegar a ' + max + '%.'
          : 'Si la humedad del suelo cae por debajo de este valor, la bomba se activa automáticamente.';
        sec.querySelector('#h2').textContent = dev
          ? 'Si la bomba lleva este tiempo encendida sin llegar al ' + max + '%, se apaga por seguridad (revisa sensor o agua).'
          : 'Tiempo que la bomba permanece encendida en cada riego.';
        sec.querySelector('#h3').textContent = dev
          ? 'Pausa de seguridad después de un corte por tiempo: ' + wtxt + '. Al terminar, el ESP32 vuelve a intentar.'
          : 'Evita ciclos de riego demasiado frecuentes tras una activación.';
      }
      d.checked = PG.state.demo;
      d.onchange = function () { PG.set('demo', d.checked); paint(); };
      r3.oninput = function () { PG.set('wait', +r3.value); paint(); };
      sec.querySelector('#r1').addEventListener('input', paint);
      PG.on('device', paint);
      function sync() {
        map.forEach(function (m) { var r = sec.querySelector('#' + m[0]), o = sec.querySelector('#' + m[1]); r.value = PG.state[m[2]]; o.textContent = r.value + m[3]; });
        d.checked = PG.state.demo; paint();
        sec.querySelector('#usub').textContent = 'Ajustes de «' + (PG.plant ? PG.plant.nombre : 'la planta') + '» · define cuándo se activa su riego automático';
      }
      PG.on('plant', sync);
      sync();
      var b = sec.querySelector('#save');
      b.onclick = function () {
        PG.sim.saveSettings();
        if (PG.plantSaveCfg) PG.plantSaveCfg();
        b.textContent = 'Cambios guardados';
        setTimeout(function () { b.textContent = 'Guardar cambios'; }, 1600);
      };
    }
  });
})();
