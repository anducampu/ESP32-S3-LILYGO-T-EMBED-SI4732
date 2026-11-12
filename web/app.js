'use strict';
/*
  T-Embed SI4732 web receiver UI.

  Talks to one HTTP API, served by tools/webui_bridge.py today and by the ESP32
  itself after the port:

    GET  /api/bands   band / step / bandwidth table
    GET  /events      SSE: "state" (full state) and "rds" (raw groups)
    POST /api/cmd     {c, v}: freq | band | mode | vol | mute | step | bw | agc | sm | seek

  RDS arrives as raw groups and is decoded here (rds.js), so the decoder is the same
  code whatever serves the page.
*/

const $ = (id) => document.getElementById(id);
const store = {
  get(k, d) { try { const v = localStorage.getItem(k); return v === null ? d : JSON.parse(v); } catch { return d; } },
  set(k, v) { try { localStorage.setItem(k, JSON.stringify(v)); } catch { /* private window */ } },
};

let BT = { bands: [], groups: {}, bw: {} };
let S = { conn: false };
const rds = new RDS.Decoder();
let rdsRegion = store.get('rdsRegion', 'EU');

const isSSB = (m) => m === 'LSB' || m === 'USB';
const modeClass = (m) => (m === 'FM' ? 'FM' : isSSB(m) ? 'SSB' : 'AM');
const band = () => BT.bands[S.band];

// ===================================================================== helpers
function fmtFreq(hz, mode) {
  if (hz == null) return ['———', ''];
  if (mode === 'FM') return [(hz / 1e6).toFixed(2), 'MHz'];
  if (isSSB(mode)) return [(hz / 1e3).toFixed(2), 'kHz'];
  return [(hz / 1e3).toFixed(0), 'kHz'];
}
const fmtStep = (hz) => (hz >= 1e6 ? hz / 1e6 + ' MHz' : hz >= 1e3 ? hz / 1e3 + ' kHz' : hz + ' Hz');

function parseFreq(txt) {
  const t = txt.trim().toLowerCase().replace(',', '.').replace(/\s+/g, '');
  const m = t.match(/^(\d+(?:\.\d+)?)(mhz|m|khz|k|hz)?$/);
  if (!m) return null;
  const n = parseFloat(m[1]), u = m[2] || '';
  if (u.startsWith('m')) return Math.round(n * 1e6);
  if (u.startsWith('k')) return Math.round(n * 1e3);
  if (u === 'hz') return Math.round(n);
  // No unit: below 30 or inside 64-108 is MHz, anything else kHz.
  return (n < 30 || (n >= 64 && n <= 108)) ? Math.round(n * 1e6) : Math.round(n * 1e3);
}

let toastTimer;
function toast(msg, ms = 3200) {
  const t = $('toast');
  t.textContent = msg; t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.hidden = true; }, ms);
}

function el(tag, attrs = {}, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === 'class') e.className = v;
    else if (k.startsWith('on')) e[k] = v;
    else if (v !== false && v != null) e.setAttribute(k, v === true ? '' : v);
  }
  for (const k of kids) if (k != null) e.append(k);
  return e;
}

function kv(dl, rows) {
  dl.replaceChildren(...rows.filter((r) => r && r[1] != null && r[1] !== '').flatMap(([k, v, mono]) =>
    [el('dt', {}, k), el('dd', { class: mono ? 'mono' : '' }, String(v))]));
}

// ===================================================================== driver
const api = {
  busy: {}, next: {},
  start() {
    const es = new EventSource('/events');
    es.addEventListener('state', (e) => onState(JSON.parse(e.data)));
    es.addEventListener('rds', (e) => onRds(JSON.parse(e.data)));
    es.onerror = () => onState({ conn: false, err: 'Lost the bridge -- is webui_bridge.py running?' });
  },
  // One request in flight per command; a burst keeps only the latest value.
  async send(c, v) {
    if (this.busy[c]) { this.next[c] = v; return; }
    this.busy[c] = true;
    try {
      const r = await fetch('/api/cmd', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ c, v }),
      });
      if (!r.ok) toast((await r.json().catch(() => ({}))).error || `HTTP ${r.status}`);
    } catch { toast('Bridge unreachable'); }
    this.busy[c] = false;
    if (c in this.next) { const nv = this.next[c]; delete this.next[c]; this.send(c, nv); }
  },
};

function cmd(c, v) {
  if (!S.conn) { toast('Not connected'); return; }
  api.send(c, v);
}

// ==================================================================== tuning
const ui = { pendingFreq: null, pendingAt: 0, drag: {}, built: null, hist: [], rdsDirty: true };

function tuneStep() {
  const b = band();
  if (!b) return 1000;
  if (modeClass(S.mode) === 'SSB') {
    const s = store.get('step.SSB', 100);
    return b.ssbSteps.includes(s) ? s : 100;
  }
  return b.steps[S.stepIdx] ?? b.steps[0];
}

function shownFreq() {
  if (ui.pendingFreq != null) {
    if (S.freq === ui.pendingFreq || performance.now() - ui.pendingAt > 1500) ui.pendingFreq = null;
    else return ui.pendingFreq;
  }
  return S.freq;
}

let tuneTimer = null;
function tuneTo(hz) {
  const b = band();
  if (b) hz = Math.max(b.min, Math.min(b.max, hz));
  ui.pendingFreq = hz; ui.pendingAt = performance.now();
  render();
  // Throttle: a wheel spin sends at most ~16 tunes/s, always ending on the last.
  if (tuneTimer) return;
  tuneTimer = setTimeout(() => { tuneTimer = null; if (ui.pendingFreq != null) cmd('freq', ui.pendingFreq); }, 60);
}

function stepBy(dir) {
  const f = shownFreq();
  if (f == null || !S.conn || S.seeking) return;
  const st = tuneStep();
  tuneTo(dir > 0 ? (Math.floor(f / st) + 1) * st : (Math.ceil(f / st) - 1) * st);
}

function onState(st) {
  const prev = S;
  S = { ...S, ...st };
  if (prev.mode && S.mode && prev.mode !== S.mode && isSSB(S.mode) && !isSSB(prev.mode)) toast(`${S.mode}: SSB patch loaded`, 1800);
  if (S.mode !== 'FM' && prev.mode === 'FM') { rds.reset(); ui.rdsDirty = true; }
  render();
}

function onRds(m) {
  if (m.reset) rds.reset();
  const now = performance.now();
  for (const g of m.g) rds.push(g, now, S.freq);
  // A reset carries the station's backlog, all stamped "now": keep it out of the rate.
  if (m.reset && m.g.length > 1) rds.times = [now];
  ui.rdsDirty = true;
}

// ====================================================================== static
const PLAN = [ // kHz, IARU Region 1 + broadcast meter bands
  [148.5, 283.5, 'LW broadcast'], [526.5, 1606.5, 'MW broadcast'],
  [1810, 2000, '160 m ham'], [1810, 1838, 'CW'], [1838, 1843, 'Digital'],
  [2300, 2495, '120 m broadcast'], [3200, 3400, '90 m broadcast'],
  [3500, 3800, '80 m ham'], [3500, 3570, 'CW'], [3570, 3600, 'Digital'], [3600, 3800, 'Phone (LSB)'],
  [3900, 4000, '75 m broadcast'], [4750, 5060, '60 m broadcast'], [5351.5, 5366.5, '60 m ham'],
  [5900, 6200, '49 m broadcast'],
  [7000, 7200, '40 m ham'], [7000, 7040, 'CW'], [7040, 7060, 'Digital'], [7060, 7200, 'Phone (LSB)'],
  [7200, 7450, '41 m broadcast'], [9400, 9900, '31 m broadcast'],
  [10100, 10150, '30 m ham (CW / digital)'],
  [11600, 12100, '25 m broadcast'], [13570, 13870, '22 m broadcast'],
  [14000, 14350, '20 m ham'], [14000, 14070, 'CW'], [14070, 14099, 'Digital'], [14101, 14350, 'Phone (USB)'],
  [15100, 15800, '19 m broadcast'], [17480, 17900, '16 m broadcast'], [18068, 18168, '17 m ham'],
  [18900, 19020, '15 m broadcast'],
  [21000, 21450, '15 m ham'], [21000, 21070, 'CW'], [21070, 21110, 'Digital'], [21151, 21450, 'Phone (USB)'],
  [21450, 21850, '13 m broadcast'], [24890, 24990, '12 m ham'], [25670, 26100, '11 m broadcast'],
  [26965, 27405, 'CB (40 ch)'],
  [28000, 29700, '10 m ham'], [28000, 28070, 'CW'], [28070, 28190, 'Digital'], [28225, 29700, 'Phone (USB)'],
  [87500, 108000, 'FM broadcast'], [76000, 90000, 'FM broadcast (Japan)'], [65800, 74000, 'OIRT FM'],
];
const FT8 = [1840, 3573, 5357, 7074, 10136, 14074, 18100, 21074, 24915, 28074];

function planFor(hz) {
  const k = hz / 1e3;
  const out = PLAN.filter(([lo, hi]) => k >= lo && k <= hi).sort((a, b) => (b[1] - b[0]) - (a[1] - a[0])).map((h) => h[2]);
  const ft = FT8.find((d) => k >= d - 0.2 && k <= d + 3.2);
  if (ft) out.push(`FT8 (dial ${ft} kHz, USB)`);
  return out;
}

const BW_HINT = { FM: 'Auto lets the tuner choose', AM: 'Narrower cuts splatter from neighbours', SSB: '0.5 – 1.0 kHz for CW' };

function meterScale(cls) {
  // dBuV axis, -20..80. S9 = 34 dBuV on HF (50 ohm), 6 dB per S unit.
  return cls === 'FM'
    ? [[0, '0'], [20, '20'], [40, '40'], [60, '60 dBµV']]
    : [[-14, 'S1'], [-2, 'S3'], [10, 'S5'], [22, 'S7'], [34, 'S9'], [54, '+20'], [74, '+40']];
}
const dbPct = (db) => Math.max(0, Math.min(100, db + 20));

function sText(db, cls) {
  if (db == null) return ['—', ''];
  if (cls === 'FM') return [`${db}`, 'dBµV'];
  if (db <= 34) return [`S${Math.max(0, Math.round(9 + (db - 34) / 6))}`, `${db} dBµV`];
  return [`S9+${db - 34}`, `${db} dBµV`];
}

const agcText = (v) => (v == null ? '' : v === 0 ? 'AGC on' : v === 1 ? 'AGC off' : `Att ${v - 1}`);

function setChecked(container, pred) {
  for (const c of container.children) c.setAttribute('aria-checked', String(pred(c)));
}

// ====================================================================== render
function render() {
  document.body.classList.toggle('disconnected', !S.conn);
  const pill = $('connPill');
  if (!S.conn) { pill.className = 'pill off'; pill.textContent = S.err ? 'Disconnected' : 'Not connected'; pill.title = S.err || ''; }
  else if (S.err) { pill.className = 'pill bad'; pill.textContent = 'Problem'; pill.title = S.err; }
  else { pill.className = 'pill ok'; pill.textContent = `USB · ${S.port || ''}`; pill.title = ''; }

  const b = band(), cls = modeClass(S.mode), fm = S.mode === 'FM';
  $('bandName').textContent = b ? b.name : '—';
  $('bandDesc').textContent = b ? b.desc : '';
  const badge = $('modeBadge');
  badge.textContent = S.mode || '—';
  badge.className = 'badge' + (cls === 'SSB' ? ' ssb' : '');

  const f = shownFreq();
  const [fv, fu] = fmtFreq(f, S.mode);
  $('freqVal').textContent = fv; $('freqUnit').textContent = fu;
  $('freqDisplay').classList.toggle('pending', ui.pendingFreq != null);
  $('seekNote').hidden = !S.seeking;
  $('bfoNote').hidden = cls !== 'SSB';
  $('bfoNote').textContent = cls === 'SSB' ? `BFO ${S.bfo > 0 ? '+' : ''}${S.bfo ?? 0} Hz` : '';

  // Rebuild what depends on band and mode class only when that changes.
  const key = cls + ':' + (b ? b.idx : '');
  if (ui.built !== key) {
    ui.built = key;
    const steps = b ? (cls === 'SSB' ? b.ssbSteps : b.steps) : [];
    $('steps').replaceChildren(...steps.map((s, i) => el('button', {
      class: 'chip', role: 'radio', 'data-i': i, 'data-v': s,
      onclick: () => { if (cls === 'SSB') { store.set('step.SSB', s); render(); } else cmd('step', i); },
    }, fmtStep(s))));
    $('bws').replaceChildren(...(BT.bw[cls] || []).map((label, i) => el('button', {
      class: 'chip', role: 'radio', 'data-i': i, onclick: () => cmd('bw', i),
    }, label)));
    $('bwHint').textContent = BW_HINT[cls] || '';
    document.querySelector('.meter-ticks').replaceChildren(...meterScale(cls).map(([db, label]) =>
      el('span', { style: `left:${dbPct(db)}%` }, label)));
  }
  const st = tuneStep();
  setChecked($('steps'), (c) => +c.dataset.v === st);
  setChecked($('bws'), (c) => +c.dataset.i === S.bwIdx);

  const range = $('tuneRange');
  if (b) {
    range.min = b.min; range.max = b.max; range.step = st;
    if (!ui.drag.tune && f != null) range.value = f;
    $('rangeLo').textContent = fmtFreq(b.min, S.mode).join(' ');
    $('rangeHi').textContent = fmtFreq(b.max, S.mode).join(' ');
  }

  for (const id of ['btnSeekDown', 'btnSeekUp']) {
    const btn = $(id);
    btn.classList.toggle('active', !!S.seeking);
    btn.textContent = S.seeking ? 'Stop' : id === 'btnSeekUp' ? '▶▶' : '◀◀';
    btn.title = cls === 'SSB' ? 'Seek does not work in SSB' : S.seeking ? 'Stop seeking' : id === 'btnSeekUp' ? 'Seek up' : 'Seek down';
  }

  $('modeRow').hidden = fm || !S.mode;
  $('fmModeNote').hidden = !fm;
  setChecked($('modes'), (c) => c.dataset.v === S.mode);

  $('plan').replaceChildren(...(f != null ? planFor(f) : []).map((p) => el('span', {}, p)));

  // Signal
  $('meterFill').style.width = (S.dbuv == null ? 0 : dbPct(S.dbuv)) + '%';
  const [sv, sd] = sText(S.dbuv, cls);
  $('sUnits').textContent = sv; $('dbuv').textContent = sd;
  const stats = [['SNR', S.snr != null ? `${S.snr} dB` : '—']];
  if (fm) stats.push(
    ['Pilot', S.pilot ? 'Stereo' : 'Mono'],
    ['Blend', S.blend != null ? `${S.blend}%` : '—'],
    ['Multipath', S.multipath != null ? `${S.multipath}%` : '—'],
    ['Offset', S.freqOff != null ? `${S.freqOff > 0 ? '+' : ''}${S.freqOff} kHz` : '—']);
  $('sigStats').replaceChildren(...stats.map(([k, v]) => el('div', {}, el('dt', {}, k), el('dd', {}, v))));

  // Reception
  if (!ui.drag.agc && S.agc != null) $('agc').value = S.agc;
  $('agcVal').textContent = agcText(ui.drag.agc ? +$('agc').value : S.agc);
  $('smCtl').hidden = fm;
  if (!ui.drag.sm && S.sm != null) $('sm').value = S.sm;
  $('smVal').textContent = `${ui.drag.sm ? $('sm').value : S.sm ?? '—'} dB`;

  // Audio
  if (!ui.drag.vol && S.vol != null) $('vol').value = S.vol;
  $('volPct').textContent = S.volPct == null ? '—' : S.volPct + '%';
  $('btnMute').setAttribute('aria-pressed', String(!!S.mute));
  $('btnMute').textContent = S.mute ? 'Muted' : 'Mute';

  document.querySelectorAll('#bandGroups .chip').forEach((c) => c.setAttribute('aria-checked', String(+c.dataset.v === S.band)));
  for (const c of document.querySelectorAll('main .btn, main .chip:not([data-region]), main input, #freqForm button')) c.disabled = !S.conn;
  for (const id of ['btnSeekDown', 'btnSeekUp']) if (cls === 'SSB') $(id).disabled = true;

  $('rdsCard').hidden = !fm;
  ui.rdsDirty = true;
}

// ======================================================================== RDS
function renderRds() {
  if (!ui.rdsDirty || $('rdsCard').hidden) return;
  ui.rdsDirty = false;
  const d = rds, reg = rdsRegion;
  document.querySelectorAll('[data-region]').forEach((c) => c.setAttribute('aria-checked', String(c.dataset.region === reg)));

  const ps = d.psConfirmed || d.ps;
  $('psInline').textContent = S.mode === 'FM' ? (ps || '') : '';
  $('rdsEmpty').hidden = d.total > 0;
  $('rdsBody').hidden = d.total === 0;
  if (!d.total) return;

  $('rdsPs').textContent = ps || '—';
  $('rdsLps').textContent = d.lps ? `Long name: ${d.lps}` : '';

  const flag = (on, label, cls = 'on') => el('span', { class: 'flag' + (on ? ' ' + cls : ''), title: label }, label);
  const flags = [];
  if (d.pty != null) flags.push(el('span', { class: 'flag on' }, `PTY ${d.pty} · ${d.ptyName(reg)}`));
  if (d.tp != null) flags.push(flag(d.tp, 'TP traffic programme'));
  if (d.ta != null) flags.push(flag(d.ta && d.tp, d.ta && d.tp ? 'TA traffic announcement now' : 'TA', 'alert'));
  if (d.ms) flags.push(flag(true, d.ms));
  if (d.di.stereo != null) flags.push(flag(d.di.stereo, d.di.stereo ? 'DI stereo' : 'DI mono'));
  if (d.di.head) flags.push(flag(true, 'Artificial head'));
  if (d.di.compressed) flags.push(flag(true, 'Compressed'));
  if (d.di.dynPty) flags.push(flag(true, 'Dynamic PTY'));
  $('rdsFlags').replaceChildren(...flags);

  const pi = d.piInfo(reg);
  kv($('rdsIdent'), [
    ['PI code', pi && pi.hex, true],
    ['Country', pi && (pi.country || (pi.candidates.length ? `one of ${pi.candidates.join(', ')} (no ECC sent yet)` : `code ${pi.countryNibble}`))],
    ['Coverage', pi && pi.coverage],
    ['Programme ref.', pi && pi.ref],
    reg === 'NA' ? ['Call letters', pi && (pi.call || '—')] : null,
    ['ECC', d.eccHex, true],
    ['PTY', d.pty != null ? `${d.pty} · ${d.ptyName(reg)}` : null],
    ['PTYN', d.ptyn],
    ['Language', d.language],
    ['Programme item', d.pin ? `day ${d.pin.day}, ${String(d.pin.hr).padStart(2, '0')}:${String(d.pin.min).padStart(2, '0')}` : null],
    ['TMC service', d.tmcId != null ? d.tmcId.toString(16).toUpperCase() : null, true],
  ]);

  $('rdsRt').textContent = d.rt || '—';
  $('rdsRtDone').textContent = d.rtComplete && d.rtComplete !== d.rt ? `Last complete: ${d.rtComplete}` : d.rtVer === 2 ? 'RadioText version B (32 characters)' : '';
  const items = d.rtPlusItems;
  $('rdsRtPlusWrap').hidden = !items.length;
  kv($('rdsRtPlus'), items.map((i) => [i.label, i.text]));

  $('rdsClockWrap').hidden = !d.ct;
  if (d.ct) {
    const local = new Date(d.ct.utc.getTime() + d.ct.offset * 60000);
    const p2 = (n) => String(n).padStart(2, '0');
    const off = d.ct.offset;
    const offTxt = `UTC${off >= 0 ? '+' : '−'}${Math.floor(Math.abs(off) / 60)}${Math.abs(off) % 60 ? ':30' : ''}`;
    const ago = Math.round((Date.now() - d.ct.at) / 60000);
    $('rdsClock').textContent = `${local.getUTCFullYear()}-${p2(local.getUTCMonth() + 1)}-${p2(local.getUTCDate())} ` +
      `${p2(local.getUTCHours())}:${p2(local.getUTCMinutes())} ${offTxt}  ·  ${p2(d.ct.utc.getUTCHours())}:${p2(d.ct.utc.getUTCMinutes())} UTC` +
      (ago > 0 ? `  ·  sent ${ago} min ago` : '');
  }

  const afs = d.afList;
  $('rdsAfWrap').hidden = !afs.length;
  const method = d.afMethod;
  $('rdsAfMeta').textContent = `${afs.length} heard` + (d.afExpect ? `, ${d.afExpect} announced` : '') + (method ? ` · method ${method}` : '');
  $('rdsAf').replaceChildren(...afs.map((a) => {
    const label = a.band === 'FM' ? (a.khz / 1000).toFixed(1) : `${a.khz} kHz`;
    const hz = a.khz * 1000;
    return el('button', {
      class: 'chip', title: `Tune ${label}${a.band === 'FM' ? ' MHz' : ''}`, disabled: !S.conn,
      'aria-checked': String(Math.abs(hz - (S.freq || 0)) < 50000),
      onclick: () => { ui.pendingFreq = hz; ui.pendingAt = performance.now(); cmd('freq', hz); },
    }, label);
  }));

  const eon = d.eonList;
  $('rdsEonWrap').hidden = !eon.length;
  $('rdsEon').tBodies[0].replaceChildren(...eon.map((e) => el('tr', {},
    el('td', { class: 'mono' }, e.piHex),
    el('td', { class: 'mono' }, e.psText || '—'),
    el('td', {}, e.pty != null ? d.ptyName(reg, e.pty) : '—'),
    el('td', {}, `${e.tp ? 'TP' : '—'} / ${e.ta ? 'TA' : '—'}`),
    el('td', { class: 'mono' }, [...e.af, ...e.mapped].sort((a, b) => a - b).map((k) => (k / 1000).toFixed(1)).join(' ') || '—'))));

  $('rdsOdaWrap').hidden = !d.oda.size;
  kv($('rdsOda'), [...d.oda].map(([g, o]) => [g, `${o.name} (AID ${RDS.hex4(o.aid)})`]));

  $('rdsRate').textContent = `${d.rate.toFixed(1)} groups/s · ${d.total} received · ${Math.round(d.corrected * 100 / d.total)}% needed error correction`;
  const max = Math.max(1, ...Object.values(d.counts));
  const cols = [];
  for (let t = 0; t < 16; t++) {
    const a = d.counts[t + 'A'], bb = d.counts[t + 'B'];
    cols.push(el('div', { class: 'hcol', title: `${t}A: ${a}   ${t}B: ${bb}` },
      el('div', { class: 'bar b', style: `height:${bb / max * 48}px` }),
      el('div', { class: 'bar', style: `height:${a / max * 48}px` }),
      el('div', { class: 'lbl' }, String(t))));
  }
  $('rdsHist').replaceChildren(...cols);

  if ($('rdsRawWrap').open) {
    $('rdsRaw').textContent = d.log.slice().reverse().map((g) => {
      const h = g.hex;
      return `${g.name.padEnd(4)} ${h.slice(0, 4)} ${h.slice(4, 8)} ${h.slice(8, 12)} ${h.slice(12, 16)}  ble ${h.slice(16)}`;
    }).join('\n');
  }
}

// ================================================================== sparkline
function sample() {
  const now = performance.now();
  if (S.conn && S.dbuv != null) ui.hist.push([now, S.dbuv]);
  while (ui.hist.length && now - ui.hist[0][0] > 60000) ui.hist.shift();
  drawSpark(now);
}

function drawSpark(now) {
  const c = $('spark'), dpr = window.devicePixelRatio || 1;
  const w = c.clientWidth, h = 56;
  if (!w) return;
  if (c.width !== w * dpr) { c.width = w * dpr; c.height = h * dpr; }
  const g = c.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, w, h);
  const css = getComputedStyle(document.documentElement);
  g.strokeStyle = css.getPropertyValue('--line'); g.lineWidth = 1;
  g.beginPath(); g.moveTo(0, h - 0.5); g.lineTo(w, h - 0.5); g.stroke();
  if (ui.hist.length < 2) return;
  const x = (t) => w - (now - t) / 60000 * w;
  const y = (db) => h - 2 - dbPct(db) / 100 * (h - 4);
  g.strokeStyle = css.getPropertyValue('--accent'); g.lineWidth = 1.6; g.lineJoin = 'round';
  g.beginPath();
  ui.hist.forEach(([t, db], i) => (i ? g.lineTo(x(t), y(db)) : g.moveTo(x(t), y(db))));
  g.stroke();
}

// ====================================================================== build
function slider(id, key, send) {
  const s = $(id);
  let timer = null;
  s.addEventListener('pointerdown', () => { ui.drag[key] = true; });
  s.addEventListener('input', () => {
    render();
    if (!timer) timer = setTimeout(() => { timer = null; send(+s.value); }, 90);
  });
  s.addEventListener('change', () => { ui.drag[key] = false; send(+s.value); });
}

function buildStatic() {
  $('modes').replaceChildren(...['AM', 'LSB', 'USB'].map((m) => el('button', {
    class: 'chip', role: 'radio', 'data-v': m,
    onclick: () => { if (m !== S.mode) { if (isSSB(m) && !isSSB(S.mode)) toast('Loading SSB patch…', 1500); cmd('mode', m); } },
  }, m)));

  $('bandGroups').replaceChildren(...Object.entries(BT.groups).map(([title, names]) => el('div', { class: 'r' },
    el('span', { class: 'rl' }, title),
    el('div', { class: 'chips', role: 'radiogroup', 'aria-label': title }, ...names.map((n) => {
      const b = BT.bands.find((x) => x.name === n);
      if (!b) return null;
      const m = b.type === 'FM' ? 'FM' : 'AM';
      return el('button', {
        class: 'chip', role: 'radio', 'data-v': b.idx, onclick: () => cmd('band', b.idx),
        title: `${b.desc}: ${fmtFreq(b.min, m).join(' ')} – ${fmtFreq(b.max, m).join(' ')}`,
      }, b.name);
    })))));

  $('btnUp').onclick = () => stepBy(+1);
  $('btnDown').onclick = () => stepBy(-1);
  $('btnSeekUp').onclick = () => cmd('seek', S.seeking ? 'stop' : 'up');
  $('btnSeekDown').onclick = () => cmd('seek', S.seeking ? 'stop' : 'down');

  const range = $('tuneRange');
  range.addEventListener('pointerdown', () => { ui.drag.tune = true; });
  range.addEventListener('input', () => tuneTo(+range.value));
  range.addEventListener('change', () => { ui.drag.tune = false; });
  window.addEventListener('pointerup', () => { ui.drag = {}; });

  let wheelAcc = 0;
  $('freqDisplay').addEventListener('wheel', (e) => {
    e.preventDefault();
    wheelAcc += e.deltaY;
    while (Math.abs(wheelAcc) >= 40) { stepBy(wheelAcc < 0 ? +1 : -1); wheelAcc -= Math.sign(wheelAcc) * 40; }
  }, { passive: false });

  $('freqDisplay').onclick = () => {
    if (!S.conn) return;
    $('freqDisplay').hidden = true; $('freqForm').hidden = false;
    const i = $('freqInput'); i.value = ''; i.focus();
  };
  const closeForm = () => { $('freqForm').hidden = true; $('freqDisplay').hidden = false; };
  $('freqInput').addEventListener('keydown', (e) => { if (e.key === 'Escape') closeForm(); });
  $('freqInput').addEventListener('blur', () => setTimeout(closeForm, 150));
  $('freqForm').onsubmit = (e) => {
    e.preventDefault();
    const hz = parseFreq($('freqInput').value);
    closeForm();
    if (hz == null) return toast('Could not read that frequency');
    if (!BT.bands.some((b) => hz >= b.min && hz <= b.max)) return toast('Out of range: 150 kHz – 30 MHz or 64 – 108 MHz');
    ui.pendingFreq = hz; ui.pendingAt = performance.now();
    cmd('freq', hz);
  };

  document.addEventListener('keydown', (e) => {
    const t = e.target;
    if (t.tagName === 'INPUT' || t.tagName === 'SUMMARY' || e.ctrlKey || e.metaKey || e.altKey) return;
    const k = { ArrowRight: 1, ArrowUp: 1, ArrowLeft: -1, ArrowDown: -1, PageUp: 10, PageDown: -10 }[e.key];
    if (!k) return;
    e.preventDefault();
    for (let i = 0; i < Math.abs(k); i++) stepBy(Math.sign(k));
  });

  slider('vol', 'vol', (v) => cmd('vol', v));
  $('vol').addEventListener('input', () => { $('volPct').textContent = Math.round($('vol').value * 100 / 63) + '%'; });
  slider('agc', 'agc', (v) => cmd('agc', v));
  slider('sm', 'sm', (v) => cmd('sm', v));
  $('btnMute').onclick = () => cmd('mute', !S.mute);

  document.querySelectorAll('[data-region]').forEach((c) => {
    c.onclick = () => { rdsRegion = c.dataset.region; store.set('rdsRegion', rdsRegion); ui.rdsDirty = true; };
  });
  $('rdsRawWrap').addEventListener('toggle', () => { ui.rdsDirty = true; });

  const clock = () => { $('utc').textContent = new Date().toISOString().slice(11, 16) + ' UTC'; };
  clock(); setInterval(clock, 10000);
  setInterval(sample, 500);
  setInterval(renderRds, 250);
  window.addEventListener('resize', () => drawSpark(performance.now()));
}

// ======================================================================= boot
async function boot() {
  try {
    BT = await (await fetch('/api/bands', { cache: 'no-store' })).json();
  } catch {
    S.err = 'No API here. Start the page with: python3 tools/webui_bridge.py';
    toast(S.err, 10000);
    return;
  }
  buildStatic();
  render();
  api.start();
}

boot();
