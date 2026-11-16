'use strict';
/* Smart Parking dashboard (plain JavaScript, no build step, no external libraries). */

const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
const esc = (v) => String(v ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const fmtTime = (ms) => new Date(ms).toLocaleString([], { month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit', second: '2-digit' });
const fmtClock = (ms) => new Date(ms).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });

const REASONS = {
  unknown_card: 'Card is not registered',
  blocked_card: 'Card is blocked',
  lot_full: 'Parking lot is full',
  cache_not_ready: 'Gate controller has no card list yet',
};

let state = null;       // latest /api/state
let sim = null;         // latest simulator snapshot
let cards = [];
let evLimit = 100;
let statsRange = 'today';
let stream = null;

// ---- small helpers ---------------------------------------------------------
function toast(message, bad = false) {
  const el = document.createElement('div');
  el.className = `toast${bad ? ' bad' : ''}`;
  el.textContent = message;
  $('#toasts').append(el);
  setTimeout(() => el.remove(), bad ? 6000 : 3500);
}

async function api(method, url, body) {
  const res = await fetch(url, {
    method,
    headers: body === undefined ? {} : { 'Content-Type': 'application/json' },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  if (res.status === 401 && url !== '/api/login') {
    showLogin();
    throw new Error('Your session has ended. Sign in again.');
  }
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(data.error || `Request failed (${res.status}).`);
  return data;
}

/** Runs an action and reports the outcome as a toast. */
async function act(fn, done) {
  try {
    await fn();
    if (done) toast(done);
    return true;
  } catch (err) {
    toast(err.message, true);
    return false;
  }
}

const debounce = (fn, ms) => {
  let t;
  return (...a) => { clearTimeout(t); t = setTimeout(() => fn(...a), ms); };
};

// ---- sign in ---------------------------------------------------------------
function showLogin() {
  if (stream) { stream.close(); stream = null; }
  $('#app').hidden = true;
  $('#login').hidden = false;
  $('#login-password').focus();
}

$('#login-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  $('#login-error').hidden = true;
  try {
    await api('POST', '/api/login', { password: $('#login-password').value });
    $('#login-password').value = '';
    await boot();
  } catch (err) {
    $('#login-error').textContent = err.message;
    $('#login-error').hidden = false;
  }
});

$('#logout').addEventListener('click', async () => {
  await api('POST', '/api/logout').catch(() => {});
  showLogin();
});

// ---- header + lot ----------------------------------------------------------
function renderState() {
  if (!state) return;
  const dev = $('#link-device');
  dev.className = `link ${state.deviceOnline ? 'ok' : 'bad'}`;
  const kind = state.device === 'simulator' ? ' (simulator)' : state.device ? ` (${state.device})` : '';
  dev.lastElementChild.textContent = state.deviceOnline ? `Gate controller online${kind}` : 'Gate controller offline';
  const broker = $('#link-broker');
  broker.className = `link ${state.broker ? 'ok' : 'bad'}`;
  broker.lastElementChild.textContent = state.broker ? 'MQTT broker connected' : 'MQTT broker unreachable';

  const messages = [];
  if (state.conflictingDevices.length > 1) {
    messages.push(`Two gate controllers are reporting at once (${state.conflictingDevices.join(' and ')}). Switch the simulator off in the Test bench tab while the real ESP32 is connected.`);
  }
  if (!state.deviceOnline) {
    messages.push('The gate controller is offline. It keeps working on its own and will deliver its buffered events when the connection returns. The numbers below are the last ones it reported.');
  } else if (state.queued > 0) {
    messages.push(`The gate controller is delivering ${state.queued} buffered event${state.queued === 1 ? '' : 's'}.`);
  }
  $('#banner').hidden = messages.length === 0;
  $('#banner').textContent = messages.join(' ');

  $('#free-count').textContent = state.free;
  $('#total-count').textContent = state.total;
  $('.lot-count').classList.toggle('full', state.free === 0);

  const bays = $('#bays');
  if (state.total <= 60) {
    bays.className = 'bays';
    const rows = Math.ceil(state.total / 20);
    bays.style.gridTemplateColumns = `repeat(${Math.ceil(state.total / rows)}, minmax(0, 1fr))`;
    bays.innerHTML = Array.from({ length: state.total }, (_, i) => `<div class="bay${i < state.occupied ? ' taken' : ''}"></div>`).join('');
  } else {
    bays.className = 'capacity-bar';
    bays.style.gridTemplateColumns = '';
    bays.innerHTML = `<i style="width:${(state.occupied / state.total) * 100}%"></i>`;
  }
  bays.setAttribute('aria-label', `${state.occupied} of ${state.total} spaces occupied`);
  $('#lot-note').textContent = `${state.occupied} occupied. The gates count vehicles in and out, so bays fill from the left rather than showing where each car is parked.`;

  for (const gate of ['entry', 'exit']) {
    const value = state[`${gate}Gate`];
    const el = $(`#gate-${gate}`);
    el.className = `gate ${value}`;
    $('.gate-state', el).textContent = value === 'open' ? 'Barrier open' : value === 'closed' ? 'Barrier closed' : 'No contact';
    $('button', el).disabled = !state.deviceOnline || value === 'open';
  }

  $('#tab-bench').hidden = !state.simulatorAvailable;
  renderDeviceInfo();
}

function renderDeviceInfo() {
  const rows = [
    ['Connection', state.deviceOnline ? 'Online' : 'Offline'],
    ['Device', state.device || 'Not seen yet'],
    ['Last report', state.statusAt ? fmtTime(state.statusAt) : 'Never'],
    ['IP address', state.ip || '–'],
    ['Wi-Fi signal', state.rssi ? `${state.rssi} dBm` : '–'],
    ['Running for', state.uptime === null ? '–' : `${Math.floor(state.uptime / 3600)} h ${Math.floor((state.uptime % 3600) / 60)} min`],
    ['Card list on device', state.deviceCacheVersion === null || state.deviceCacheVersion < 0
      ? 'None yet'
      : `Version ${state.deviceCacheVersion}${state.deviceCacheVersion === state.cardsVersion ? ' (up to date)' : ` (latest is ${state.cardsVersion})`}`],
    ['Events buffered on device', state.queued],
    ['ESP32 connects to', `${location.hostname}:${state.mqttPort} (${state.embeddedBroker ? 'built-in broker' : 'external broker'})`],
  ];
  $('#device-info').innerHTML = rows.map(([k, v]) => `<dt>${esc(k)}</dt><dd>${esc(v)}</dd>`).join('');
}

$$('[data-open]').forEach((btn) => btn.addEventListener('click', () => {
  const gate = btn.dataset.open;
  act(() => api('POST', `/api/gates/${gate}/open`), `Open command sent to the ${gate} gate`);
}));

// ---- tabs ------------------------------------------------------------------
const loaders = { activity: loadEvents, cards: loadCards, stats: loadStats, bench: loadBench, settings: fillSettings };
let currentTab = 'activity';

function showTab(name) {
  currentTab = name;
  $$('.tabs button').forEach((b) => b.setAttribute('aria-selected', String(b.dataset.tab === name)));
  $$('.panel').forEach((p) => { p.hidden = p.id !== `panel-${name}`; });
  loaders[name]();
}
$$('.tabs button').forEach((b) => b.addEventListener('click', () => showTab(b.dataset.tab)));

// ---- activity ---------------------------------------------------------------
function describe(e) {
  if (e.type === 'ADMIN') return ['admin', e.reason];
  if (e.type === 'DENIED') return ['denied', 'Access denied'];
  const verb = e.type === 'ENTRY' ? 'entered' : 'left';
  if (!e.passed) return ['nopass', 'Gate opened, no vehicle passed'];
  return [e.type === 'ENTRY' ? 'entry' : 'exit', `Vehicle ${verb}`];
}

async function loadEvents() {
  const params = new URLSearchParams({ limit: evLimit });
  if ($('#ev-type').value) params.set('type', $('#ev-type').value);
  if ($('#ev-search').value.trim()) params.set('search', $('#ev-search').value.trim());
  $('#ev-export').href = `/api/events.csv?${params}`;
  let data;
  try { data = await api('GET', `/api/events?${params}`); } catch { return; }
  const known = new Set(cards.map((c) => c.uid));
  $('#ev-rows').innerHTML = data.rows.map((e) => {
    const [cls, text] = describe(e);
    const notes = [];
    if (e.type === 'DENIED') notes.push(`<span class="pill stop">${esc(REASONS[e.reason] || e.reason)}</span>`);
    if (e.offline) notes.push('<span class="pill note" title="Stored in the gate controller\'s flash during an outage and delivered later">Buffered offline</span>');
    if (e.ts_estimated) notes.push('<span class="pill note" title="The gate controller had no clock when this happened, so this is when the event reached the server">Time is approximate</span>');
    if (e.type !== 'ADMIN' && e.type !== 'DENIED' && e.occupied !== null) notes.push(`<span class="muted">${esc(e.occupied)} occupied after</span>`);
    if (e.type === 'DENIED' && e.reason === 'unknown_card' && e.uid && !known.has(e.uid)) {
      notes.push(`<button class="btn" data-register="${esc(e.uid)}" type="button">Register this card</button>`);
    }
    return `<tr>
      <td class="time">${esc(fmtTime(e.ts))}</td>
      <td><span class="what ${cls}">${esc(text)}</span></td>
      <td>${esc(e.owner) || '<span class="muted">–</span>'}</td>
      <td class="uid">${esc(e.uid)}</td>
      <td>${esc(e.gate)}</td>
      <td>${notes.join(' ')}</td></tr>`;
  }).join('');
  $('#ev-empty').hidden = data.rows.length > 0 || Boolean($('#ev-type').value || $('#ev-search').value);
  $('#ev-count').textContent = data.total ? `${data.rows.length} of ${data.total}` : (data.rows.length ? '' : 'Nothing matches');
  $('#ev-more').hidden = data.rows.length >= data.total;
}
const reloadEvents = debounce(loadEvents, 150);
$('#ev-type').addEventListener('change', () => { evLimit = 100; loadEvents(); });
$('#ev-search').addEventListener('input', debounce(() => { evLimit = 100; loadEvents(); }, 250));
$('#ev-more').addEventListener('click', () => { evLimit += 200; loadEvents(); });
$('#ev-rows').addEventListener('click', (e) => {
  const btn = e.target.closest('[data-register]');
  if (btn) openCardDialog({ uid: btn.dataset.register });
});

// ---- cards -----------------------------------------------------------------
async function loadCards() {
  try { cards = await api('GET', '/api/cards'); } catch { return; }
  $('#card-rows').innerHTML = cards.map((c) => `<tr>
      <td><b>${esc(c.owner)}</b></td>
      <td class="uid">${esc(c.uid)}</td>
      <td>${esc(c.plate) || '<span class="muted">–</span>'}</td>
      <td><span class="pill ${c.status === 'active' ? 'go' : 'stop'}">${c.status === 'active' ? 'Active' : 'Blocked'}</span></td>
      <td>${c.inside ? 'Inside' : '<span class="muted">Outside</span>'}</td>
      <td>${esc(c.entries)}</td>
      <td class="time">${c.last_seen ? esc(fmtTime(c.last_seen)) : '<span class="muted">Never</span>'}</td>
      <td class="actions" data-uid="${esc(c.uid)}">
        <button class="btn" data-act="edit" type="button">Edit</button>
        <button class="btn" data-act="toggle" type="button">${c.status === 'active' ? 'Block' : 'Unblock'}</button>
        <button class="btn danger" data-act="delete" type="button">Delete</button>
      </td></tr>`).join('');
  $('#card-empty').hidden = cards.length > 0;
  renderSimCards();
}

$('#card-rows').addEventListener('click', async (e) => {
  const btn = e.target.closest('[data-act]');
  if (!btn) return;
  const card = cards.find((c) => c.uid === btn.parentElement.dataset.uid);
  if (!card) return;
  if (btn.dataset.act === 'edit') openCardDialog(card, true);
  if (btn.dataset.act === 'toggle') {
    const status = card.status === 'active' ? 'blocked' : 'active';
    await act(() => api('PUT', `/api/cards/${encodeURIComponent(card.uid)}`, { status }), status === 'blocked' ? `Blocked ${card.owner}'s card` : `Unblocked ${card.owner}'s card`);
  }
  if (btn.dataset.act === 'delete' && window.confirm(`Delete the card of ${card.owner} (${card.uid})? Its past activity stays in the log.`)) {
    await act(() => api('DELETE', `/api/cards/${encodeURIComponent(card.uid)}`), 'Card deleted');
  }
});

let editingUid = null;
function openCardDialog(card = {}, editing = false) {
  editingUid = editing ? card.uid : null;
  $('#card-title').textContent = editing ? 'Edit card' : 'Add card';
  $('#card-save').textContent = editing ? 'Save changes' : 'Add card';
  $('#card-uid').value = card.uid || '';
  $('#card-uid').disabled = editing;
  $('#card-owner').value = card.owner || '';
  $('#card-plate').value = card.plate || '';
  $('#card-error').hidden = true;
  $('#card-dialog').showModal();
  (card.uid ? $('#card-owner') : $('#card-uid')).focus();
}
$('#card-add').addEventListener('click', () => openCardDialog());
$('#card-cancel').addEventListener('click', () => $('#card-dialog').close());
$('#card-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  const body = { uid: $('#card-uid').value, owner: $('#card-owner').value, plate: $('#card-plate').value };
  try {
    if (editingUid) await api('PUT', `/api/cards/${encodeURIComponent(editingUid)}`, { owner: body.owner, plate: body.plate });
    else await api('POST', '/api/cards', body);
    $('#card-dialog').close();
    toast(editingUid ? 'Changes saved' : 'Card added and sent to the gate controller');
  } catch (err) {
    $('#card-error').textContent = err.message;
    $('#card-error').hidden = false;
  }
});

// ---- statistics --------------------------------------------------------------
function rangeStart() {
  const now = Date.now();
  if (statsRange === '24h') return now - 24 * 3600e3;
  if (statsRange === '7d') return now - 7 * 24 * 3600e3;
  const d = new Date();
  d.setHours(0, 0, 0, 0);
  return d.getTime();
}

const list = (el, items, empty) => { el.innerHTML = items.length ? items.join('') : `<li class="none">${esc(empty)}</li>`; };

async function loadStats() {
  let s;
  try { s = await api('GET', `/api/stats?from=${rangeStart()}`); } catch { return; }
  $('#st-entries').textContent = s.counts.entries;
  $('#st-exits').textContent = s.counts.exits;
  $('#st-denied').textContent = s.counts.denied;
  $('#st-buffered').textContent = s.counts.buffered;
  drawOccupancy(s);
  drawFlow(s);
  list($('#st-inside'), s.inside.map((c) => `<li><span><b>${esc(c.owner || c.uid)}</b> ${esc(c.plate || '')}</span><span class="muted">since ${esc(fmtTime(c.since))}</span></li>`), 'No registered vehicle is inside.');
  list($('#st-top'), s.topCards.map((c) => `<li><span>${esc(c.owner || c.uid)}</span><b>${esc(c.visits)}</b></li>`), 'No visits in this period.');
  list($('#st-reasons'), s.deniedReasons.map((r) => `<li><span>${esc(REASONS[r.reason] || r.reason)}</span><b>${esc(r.n)}</b></li>`), 'No scans were denied.');
}
const reloadStats = debounce(() => { if (currentTab === 'stats') loadStats(); }, 400);
$$('#stats-range button').forEach((b) => b.addEventListener('click', () => {
  statsRange = b.dataset.range;
  $$('#stats-range button').forEach((x) => x.setAttribute('aria-pressed', String(x === b)));
  loadStats();
}));

const W = 560; const H = 220; const M = { l: 34, r: 12, t: 10, b: 26 };
const tickLabel = (ms, span) => (span > 36 * 3600e3
  ? new Date(ms).toLocaleDateString([], { weekday: 'short', day: 'numeric' })
  : new Date(ms).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }));

function axes(from, to, yMax) {
  const x = (t) => M.l + ((t - from) / (to - from)) * (W - M.l - M.r);
  const y = (v) => H - M.b - (v / yMax) * (H - M.t - M.b);
  let svg = '';
  const yStep = [1, 2, 5, 10, 20, 25, 50, 100, 200].find((n) => yMax / n <= 5) || 250;
  for (let v = 0; v <= yMax; v += yStep) {
    svg += `<line x1="${M.l}" x2="${W - M.r}" y1="${y(v)}" y2="${y(v)}" stroke="#dfe4e8"/><text x="${M.l - 6}" y="${y(v) + 4}" text-anchor="end">${v}</text>`;
  }
  for (let i = 0; i <= 4; i += 1) {
    const t = from + ((to - from) / 4) * i;
    svg += `<text x="${x(t)}" y="${H - 7}" text-anchor="${i === 0 ? 'start' : i === 4 ? 'end' : 'middle'}">${esc(tickLabel(t, to - from))}</text>`;
  }
  return { x, y, svg };
}

function drawOccupancy(s) {
  const total = Math.max(state?.total || 1, ...s.occupancy.map((p) => p.total), 1);
  const { x, y, svg } = axes(s.from, s.to, total);
  if (!s.occupancy.length) {
    $('#chart-occupancy').innerHTML = '<p class="muted">No occupancy changes recorded in this period.</p>';
    return;
  }
  let d = '';
  let last = null;
  for (const p of s.occupancy) {
    d += last === null ? `M${x(p.ts)},${y(p.occupied)}` : `H${x(p.ts)}V${y(p.occupied)}`;
    last = p;
  }
  d += `H${x(s.to)}`;
  const area = `${d}V${y(0)}H${x(s.occupancy[0].ts)}Z`;
  $('#chart-occupancy').innerHTML = `<svg viewBox="0 0 ${W} ${H}" role="img" aria-label="Occupied spaces over time, currently ${last.occupied} of ${total}">
    ${svg}
    <line x1="${M.l}" x2="${W - M.r}" y1="${y(total)}" y2="${y(total)}" stroke="#c2362b" stroke-dasharray="5 4"/>
    <text x="${W - M.r}" y="${y(total) + 13}" text-anchor="end" style="fill:#c2362b">full (${total})</text>
    <path d="${area}" fill="#0a4fa322"/><path d="${d}" fill="none" stroke="#0a4fa3" stroke-width="2.5"/></svg>`;
}

function drawFlow(s) {
  const span = s.to - s.from;
  const bucket = span > 36 * 3600e3 ? 24 * 3600e3 : 3600e3;
  const first = Math.floor(s.from / bucket) * bucket;
  const n = Math.ceil((s.to - first) / bucket);
  const ins = new Array(n).fill(0);
  const outs = new Array(n).fill(0);
  for (const p of s.passages) {
    const i = Math.floor((p.ts - first) / bucket);
    if (i >= 0 && i < n) (p.type === 'ENTRY' ? ins : outs)[i] += 1;
  }
  const max = Math.max(1, ...ins, ...outs);
  const { y, svg } = axes(first, first + n * bucket, max);
  const slot = (W - M.l - M.r) / n;
  const bar = Math.max(Math.min(slot / 2 - 1.5, 14), 1.5);
  let bars = '';
  for (let i = 0; i < n; i += 1) {
    const cx = M.l + slot * i + slot / 2;
    const label = `${tickLabel(first + i * bucket, span)}: ${ins[i]} in, ${outs[i]} out`;
    if (ins[i]) bars += `<rect x="${cx - bar - 0.5}" y="${y(ins[i])}" width="${bar}" height="${y(0) - y(ins[i])}" fill="#17834f"><title>${esc(label)}</title></rect>`;
    if (outs[i]) bars += `<rect x="${cx + 0.5}" y="${y(outs[i])}" width="${bar}" height="${y(0) - y(outs[i])}" fill="#0a4fa3"><title>${esc(label)}</title></rect>`;
  }
  $('#chart-flow').innerHTML = s.passages.length
    ? `<svg viewBox="0 0 ${W} ${H}" role="img" aria-label="Vehicles in and out per ${bucket === 3600e3 ? 'hour' : 'day'}">${svg}${bars}</svg>`
    : '<p class="muted">No vehicles passed the gates in this period.</p>';
}

// ---- test bench ---------------------------------------------------------------
async function loadBench() {
  if (!cards.length) await loadCards();
  try { sim = await api('GET', '/api/sim'); } catch { return; }
  renderSim();
}

function renderSimCards() {
  const select = $('#sim-card');
  const previous = select.value;
  const options = cards.map((c) => `<option value="${esc(c.uid)}">${esc(c.owner)} (${esc(c.uid)})${c.status === 'blocked' ? ' – blocked' : ''}</option>`);
  options.push('<option value="FE:ED:FA:CE">Unregistered card (FE:ED:FA:CE)</option>');
  select.innerHTML = options.join('');
  if ([...select.options].some((o) => o.value === previous)) select.value = previous;
}

function renderSim() {
  if (!sim) return;
  const panel = $('#panel-bench');
  panel.classList.toggle('bench', true);
  panel.classList.toggle('off', !sim.running);
  $('#sim-power').checked = sim.running;
  $('#sim-network').checked = sim.network;
  $('#sim-network').disabled = !sim.running;
  for (const gate of ['entry', 'exit']) {
    const box = $(`.bench-gate[data-gate="${gate}"]`);
    const pill = $('[data-role="gate"]', box);
    pill.textContent = sim.gates[gate] === 'open' ? 'barrier open' : 'barrier closed';
    pill.className = `pill ${sim.gates[gate] === 'open' ? 'open' : ''}`;
    const [l1, l2] = sim.running ? sim.lcd[gate] : ['', ''];
    const rows = $$('[data-role="lcd"] div', box);
    rows[0].textContent = l1.padEnd(16);
    rows[1].textContent = l2.padEnd(16);
    $$('[data-sensor]', box).forEach((cb) => { cb.checked = sim.sensors[cb.dataset.sensor]; cb.disabled = !sim.running; });
    $('[data-scan]', box).disabled = !sim.running || sim.busy[gate];
  }
  $$('[data-drive]').forEach((b) => { b.disabled = !sim.running || sim.busy[b.dataset.drive]; });
  $('#sim-flash').textContent = sim.cacheVersion < 0
    ? 'No card list stored yet. The gates stay closed until the backend sends one.'
    : `Card list version ${sim.cacheVersion} with ${sim.cards.length} card${sim.cards.length === 1 ? '' : 's'}. Counter: ${sim.occupied} of ${sim.total} occupied. Clock ${sim.clockSynced ? 'synced with the backend' : 'not set'}.`;
  list($('#sim-queue'), sim.queue.map((q) => `<li><span>${esc(q.type)} at ${esc(q.gate)} gate</span><span class="uid">${esc(q.uid)}</span></li>`),
    sim.network ? 'Nothing waiting. Everything has been delivered.' : 'Nothing buffered yet. Scan a card while the network is off.');
  const log = $('#sim-log');
  const atBottom = log.scrollTop + log.clientHeight >= log.scrollHeight - 8;
  log.textContent = sim.log.map((l) => `${fmtClock(l.t)}  ${l.text}`).join('\n');
  if (atBottom) log.scrollTop = log.scrollHeight;
  const buzz = $('#sim-buzzer');
  const fresh = sim.buzzer && Date.now() - sim.buzzer.at < 2500;
  buzz.hidden = !fresh;
  if (fresh) {
    buzz.textContent = { success: 'Buzzer: one beep (accepted)', error: 'Buzzer: three beeps (denied)', open: 'Buzzer: two-tone (gate opening)' }[sim.buzzer.kind];
    buzz.className = `pill ${sim.buzzer.kind === 'error' ? 'stop' : 'go'}`;
    setTimeout(() => { if (sim.buzzer && Date.now() - sim.buzzer.at >= 2400) buzz.hidden = true; }, 2600);
  }
}

const simUid = () => $('#sim-uid').value.trim() || $('#sim-card').value;
$('#sim-power').addEventListener('change', (e) => act(() => api('POST', '/api/sim/power', { on: e.target.checked })));
$('#sim-network').addEventListener('change', (e) => act(() => api('POST', '/api/sim/network', { online: e.target.checked }),
  e.target.checked ? 'Network restored for the gate controller' : 'Network cut. The gate controller now works on its own.'));
$$('[data-sensor]').forEach((cb) => cb.addEventListener('change', () => act(() => api('POST', '/api/sim/sensor', { name: cb.dataset.sensor, present: cb.checked }))));
$$('[data-scan]').forEach((b) => b.addEventListener('click', () => act(() => api('POST', '/api/sim/scan', { gate: b.dataset.scan, uid: simUid() }))));
$$('[data-drive]').forEach((b) => b.addEventListener('click', () => act(() => api('POST', '/api/sim/drive', { gate: b.dataset.drive, uid: simUid() }))));
$('#sim-erase').addEventListener('click', () => {
  if (window.confirm('Erase the simulated flash memory (card list, buffered events and counters)? Switch the power off and on afterwards to see a first boot.')) {
    act(() => api('POST', '/api/sim/erase'), 'Flash erased');
  }
});

// ---- settings -------------------------------------------------------------------
function fillSettings() {
  if (!state) return;
  if (document.activeElement !== $('#set-total')) $('#set-total').value = state.total;
  if (document.activeElement !== $('#set-occupied')) $('#set-occupied').value = state.occupied;
}
$('#settings-form').addEventListener('submit', (e) => {
  e.preventDefault();
  const body = {};
  const total = Number($('#set-total').value);
  const occupied = Number($('#set-occupied').value);
  if (total !== state.total) body.total = total;
  if (occupied !== state.occupied || body.total !== undefined) body.occupied = occupied;
  if (!Object.keys(body).length) { toast('Nothing to change'); return; }
  act(() => api('POST', '/api/settings', body), 'Changes saved');
});
$('#push-whitelist').addEventListener('click', () => act(() => api('POST', '/api/whitelist/push'), 'Card list sent'));
$('#clear-events').addEventListener('click', () => {
  if (window.confirm('Delete the whole activity log and occupancy history? Cards are kept. This cannot be undone.')) {
    act(() => api('DELETE', '/api/events'), 'Activity log deleted');
  }
});

// ---- live updates -----------------------------------------------------------------
function connectStream() {
  if (stream) stream.close();
  stream = new EventSource('/api/stream');
  stream.addEventListener('state', (e) => {
    state = { ...state, ...JSON.parse(e.data) };
    renderState();
    if (currentTab === 'settings') fillSettings();
    reloadStats();
  });
  stream.addEventListener('activity', () => {
    if (currentTab === 'activity') reloadEvents();
    if (currentTab === 'cards') loadCards();
    reloadStats();
  });
  stream.addEventListener('cards', () => { loadCards().then(() => { if (currentTab === 'activity') reloadEvents(); }); });
  stream.addEventListener('sim', (e) => { sim = JSON.parse(e.data); if (currentTab === 'bench') renderSim(); });
  stream.onerror = async () => {
    // The browser reconnects by itself; only act if the session is gone.
    const s = await fetch('/api/session').then((r) => r.json()).catch(() => null);
    if (s && !s.authenticated) showLogin();
  };
}

async function boot() {
  const session = await fetch('/api/session').then((r) => r.json());
  $('#login-hint').hidden = !session.defaultPassword;
  if (!session.authenticated) { showLogin(); return; }
  $('#login').hidden = true;
  $('#app').hidden = false;
  state = await api('GET', '/api/state');
  renderState();
  await loadCards();
  connectStream();
  showTab(currentTab);
}

boot().catch((err) => toast(err.message, true));
