'use strict';
// End-to-end test: real broker, real database, real HTTP API and the virtual
// gate controller, all in one process. Run with:  npm test
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { start } = require('../src/app');

const sleep = (ms) => new Promise((r) => { setTimeout(r, ms); });
async function until(check, what, timeout = 6000) {
  const end = Date.now() + timeout;
  for (;;) {
    const value = await check();
    if (value) return value;
    if (Date.now() > end) throw new Error(`Timed out waiting for: ${what}`);
    await sleep(40);
  }
}

let app;
let cookie = '';
let base;
const dataDir = fs.mkdtempSync(path.join(os.tmpdir(), 'parking-test-'));

async function api(method, url, body) {
  const res = await fetch(base + url, {
    method,
    headers: { 'Content-Type': 'application/json', Cookie: cookie },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const text = await res.text();
  let json;
  try { json = JSON.parse(text); } catch { json = text; }
  return { status: res.status, body: json, headers: res.headers };
}
const events = async (q = '') => (await api('GET', `/api/events?limit=500${q}`)).body.rows;
const state = async () => (await api('GET', '/api/state')).body;

/** Drives a car through a gate on the simulator and waits until the gate is closed again. */
async function drive(gate, uid) {
  const { sim } = app;
  sim.setSensor(`${gate}Before`, true);
  const result = sim.scan(gate, uid);
  if (result.granted) {
    sim.setSensor(`${gate}After`, true);
    await sleep(30);
    sim.setSensor(`${gate}After`, false);
  }
  sim.setSensor(`${gate}Before`, false);
  return result;
}

test.before(async () => {
  app = await start({
    httpPort: 0, mqttUrl: '', mqttPort: 0, dataDir, adminPassword: 'secret', simulator: true, seedDemoData: true,
  }, { log: () => {}, simOptions: { gateTimeoutMs: 400, ackTimeoutMs: 300, heartbeatMs: 500 } });
  base = `http://127.0.0.1:${app.httpPort}`;
});

test.after(async () => {
  await app.close();
  fs.rmSync(dataDir, { recursive: true, force: true });
});

test('API needs a login', async () => {
  assert.equal((await api('GET', '/api/cards')).status, 401);
  assert.equal((await api('POST', '/api/login', { password: 'nope' })).status, 401);
  const ok = await api('POST', '/api/login', { password: 'secret' });
  assert.equal(ok.status, 200);
  cookie = ok.headers.get('set-cookie').split(';')[0];
  assert.equal((await api('GET', '/api/cards')).status, 200);
});

test('device comes online and receives the whitelist of active cards only', async () => {
  await until(async () => (await state()).deviceOnline, 'device online');
  await until(() => app.sim.cacheReady, 'whitelist on device');
  const uids = app.sim.getState().cards.map((c) => c.uid).sort();
  assert.deepEqual(uids, ['11:22:33:44', 'A1:B2:C3:D4', 'DE:AD:BE:EF']); // blocked demo card is not sent
  assert.equal((await state()).total, 10);
});

test('valid card: gate opens, car passes, occupancy and log are updated', async () => {
  const before = (await state()).occupied;
  const result = await drive('entry', 'A1:B2:C3:D4');
  assert.equal(result.granted, true);
  await until(async () => (await state()).occupied === before + 1, 'occupancy +1');
  const [last] = await until(async () => { const e = await events('&type=ENTRY'); return e.length && e; }, 'entry event');
  assert.equal(last.uid, 'A1:B2:C3:D4');
  assert.equal(last.owner, 'Jubayer Alam');
  assert.equal(last.passed, 1);
  assert.equal(last.offline, 0);
  assert.equal(last.ts_estimated, 0);
  const cards = (await api('GET', '/api/cards')).body;
  assert.equal(cards.find((c) => c.uid === 'A1:B2:C3:D4').inside, true);
});

test('gate stays open while a vehicle is under the barrier, even past the timeout', async () => {
  const { sim } = app;
  sim.scan('entry', '11:22:33:44');
  sim.setSensor('entryAfter', true);
  await sleep(700); // gate timeout is 400 ms in this test
  assert.equal(sim.getState().gates.entry, 'open');
  sim.setSensor('entryAfter', false);
  assert.equal(sim.getState().gates.entry, 'closed');
  await until(async () => (await state()).occupied === 2, 'occupancy 2');
});

test('granted but nobody drives through: logged, occupancy unchanged', async () => {
  app.sim.scan('entry', 'DE:AD:BE:EF');
  await until(() => app.sim.getState().gates.entry === 'closed', 'gate closes after timeout');
  const row = await until(async () => (await events('&type=ENTRY')).find((e) => e.uid === 'DE:AD:BE:EF'), 'event');
  assert.equal(row.passed, 0);
  assert.equal((await state()).occupied, 2);
});

test('unknown and blocked cards are denied and reported', async () => {
  assert.equal((await drive('entry', 'FF:FF:FF:FF')).reason, 'unknown_card');
  assert.equal((await drive('entry', '0A:1B:2C:3D')).reason, 'unknown_card'); // device only knows "not on whitelist"
  const denied = await until(async () => { const e = await events('&type=DENIED'); return e.length === 2 && e; }, 'denied events');
  assert.deepEqual(denied.map((e) => e.reason).sort(), ['blocked_card', 'unknown_card']);
  assert.equal((await state()).occupied, 2);
});

test('card management reaches the device', async () => {
  assert.equal((await api('POST', '/api/cards', { uid: 'zz', owner: 'X' })).status, 400);
  assert.equal((await api('POST', '/api/cards', { uid: 'a1b2c3d4', owner: 'Dup' })).status, 409);
  assert.equal((await api('POST', '/api/cards', { uid: 'ca fe ba be', owner: 'New | Driver', plate: 'NEW 001' })).status, 201);
  await until(() => app.sim.getState().cards.some((c) => c.uid === 'CA:FE:BA:BE'), 'new card on device');
  assert.equal(app.sim.getState().cards.find((c) => c.uid === 'CA:FE:BA:BE').owner, 'New Driver');

  assert.equal((await api('PUT', '/api/cards/CA:FE:BA:BE', { status: 'blocked' })).status, 200);
  await until(() => !app.sim.getState().cards.some((c) => c.uid === 'CA:FE:BA:BE'), 'blocked card removed from device');
  assert.equal((await drive('entry', 'CA:FE:BA:BE')).granted, false);

  assert.equal((await api('DELETE', '/api/cards/CA:FE:BA:BE')).status, 200);
  assert.equal((await api('DELETE', '/api/cards/CA:FE:BA:BE')).status, 404);
});

test('exit lowers occupancy', async () => {
  assert.equal((await drive('exit', 'A1:B2:C3:D4')).granted, true);
  await until(async () => (await state()).occupied === 1, 'occupancy 1');
  const cards = (await api('GET', '/api/cards')).body;
  assert.equal(cards.find((c) => c.uid === 'A1:B2:C3:D4').inside, false);
  assert.equal(cards.find((c) => c.uid === '11:22:33:44').inside, true);
});

test('remote gate open and admin corrections', async () => {
  assert.equal((await api('POST', '/api/gates/exit/open')).status, 200);
  await until(() => app.sim.getState().gates.exit === 'open', 'exit gate opens');
  await until(async () => (await state()).exitGate === 'open', 'dashboard sees open gate');
  await until(() => app.sim.getState().gates.exit === 'closed', 'exit gate closes');

  assert.equal((await api('POST', '/api/settings', { total: 3, occupied: 3 })).status, 200);
  await until(async () => { const s = await state(); return s.total === 3 && s.occupied === 3; }, 'new capacity');
  assert.equal((await drive('entry', 'A1:B2:C3:D4')).reason, 'lot_full');
  assert.equal((await api('POST', '/api/settings', { occupied: 9 })).status, 400);
  assert.equal((await api('POST', '/api/settings', { total: 10, occupied: 1 })).status, 200);
  await until(async () => { const s = await state(); return s.total === 10 && s.occupied === 1; }, 'capacity restored');
  assert.ok((await events('&type=ADMIN')).length >= 4);
});

test('network outage: gates keep working, events are buffered and synced without loss or duplicates', async () => {
  const { sim } = app;
  const countBefore = (await events()).length;
  sim.setNetwork(false);
  await until(async () => !(await state()).deviceOnline, 'backend notices device offline (last will)');
  assert.equal((await api('POST', '/api/gates/entry/open')).status, 409);

  assert.equal((await drive('entry', 'A1:B2:C3:D4')).granted, true);   // whitelist is cached on the device
  assert.equal((await drive('entry', 'FF:FF:FF:FF')).granted, false);
  assert.equal((await drive('exit', '11:22:33:44')).granted, true);
  assert.equal(sim.getState().queue.length, 3);
  assert.equal(sim.getState().occupied, 1);
  assert.equal((await events()).length, countBefore);
  assert.match(sim.getState().lcd.exit[0], /\*$/); // offline marker on the LCD

  // A card registered during the outage reaches the device after reconnect.
  assert.equal((await api('POST', '/api/cards', { uid: '01:02:03:04', owner: 'Late Card' })).status, 201);

  sim.setNetwork(true);
  await until(() => sim.getState().queue.length === 0, 'queue flushed after ack');
  await until(() => sim.getState().cards.some((c) => c.uid === '01:02:03:04'), 'whitelist refreshed');
  const all = await events();
  assert.equal(all.length, countBefore + 3);
  assert.equal(all.filter((e) => e.offline).length, 3);
  assert.equal(new Set(all.map((e) => e.event_id).filter(Boolean)).size, all.filter((e) => e.event_id).length);
  assert.equal((await state()).deviceOnline, true);
  assert.equal((await state()).queued, 0);
});

test('duplicate deliveries are stored once', async () => {
  const before = (await events()).length;
  const dup = JSON.stringify({ id: 'dup-1', uid: 'A1:B2:C3:D4', gate: 'entry', passed: true, ts: Math.floor(Date.now() / 1000) });
  app.parking.client.publish('parking/entry_log', dup);
  app.parking.client.publish('parking/entry_log', dup);
  await until(async () => (await events()).length === before + 1, 'one stored');
  await sleep(150);
  assert.equal((await events()).length, before + 1);
});

test('statistics and CSV export', async () => {
  const stats = (await api('GET', `/api/stats?from=${Date.now() - 3600_000}`)).body;
  assert.ok(stats.counts.entries >= 3);
  assert.ok(stats.counts.denied >= 4);
  assert.ok(stats.occupancy.length >= 3);
  const file = await api('GET', '/api/events.csv');
  assert.match(file.headers.get('content-type'), /text\/csv/);
  assert.match(file.body, /^time,type,gate,uid,owner/);
});

test('device state survives a power cycle', async () => {
  const { sim } = app;
  const occupied = sim.getState().occupied;
  await sim.stop();
  await until(async () => !(await state()).deviceOnline, 'offline after power off');
  sim.loadFlash();
  sim.start();
  await until(async () => (await state()).deviceOnline, 'online again');
  assert.equal(sim.getState().occupied, occupied);
  assert.ok(sim.getState().cards.length >= 3);
});
