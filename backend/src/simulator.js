'use strict';
// A virtual gate controller. It speaks exactly the same MQTT protocol as the
// ESP32 firmware (src/main.cpp) and follows the same logic: local whitelist
// cache, edge-first gate decisions, store & forward queue, safe gate closing.
// It lets you test the whole stack from the dashboard without any hardware.
const fs = require('node:fs');
const path = require('node:path');
const { EventEmitter } = require('node:events');
const mqtt = require('mqtt');
const T = require('./topics');

const DEVICE = 'simulator';
const CAR_NEAR_CM = 15;

class VirtualDevice extends EventEmitter {
  constructor({ url, dataDir, gateTimeoutMs = 15_000, ackTimeoutMs = 5_000, heartbeatMs = 30_000 }) {
    super();
    this.url = url;
    this.file = path.join(dataDir, 'simulator-flash.json');
    this.gateTimeoutMs = gateTimeoutMs;
    this.ackTimeoutMs = ackTimeoutMs;
    this.heartbeatMs = heartbeatMs;
    this.running = false;
    this.timers = new Set();
    this.resetVolatile();
    this.loadFlash();
  }

  // ---- "flash memory" (survives restarts, like LittleFS + NVS) -------------
  loadFlash() {
    let saved = {};
    try { saved = JSON.parse(fs.readFileSync(this.file, 'utf8')); } catch { /* first run */ }
    this.flash = {
      boot: saved.boot ?? 0,
      total: saved.total ?? 10,
      occupied: saved.occupied ?? null,   // null = never stored, ask the backend
      cacheVersion: saved.cacheVersion ?? -1,
      cards: saved.cards ?? {},           // uid -> owner
      queue: saved.queue ?? [],           // events waiting for the backend
    };
  }

  saveFlash() {
    try { fs.writeFileSync(this.file, JSON.stringify(this.flash)); } catch { /* ignore in tests */ }
  }

  resetVolatile() {
    this.network = true;       // the simulated Wi-Fi/broker link
    this.mqttOnline = false;
    this.timeOffset = null;    // epoch ms - uptime ms, learned from parking/time
    this.seq = 0;
    this.pending = new Map();  // event id -> timer waiting for the ack
    this.sensors = { entryBefore: false, entryAfter: false, exitBefore: false, exitAfter: false };
    this.gates = { entry: 'closed', exit: 'closed' };
    this.busy = { entry: null, exit: null };
    this.lcdHold = { entry: null, exit: null };
    this.lcd = { entry: ['', ''], exit: ['', ''] };
    this.buzzer = null;
    this.log = [];
  }

  // ---- lifecycle -----------------------------------------------------------
  start() {
    if (this.running) return;
    this.resetVolatile();
    this.running = true;
    this.bootAt = Date.now();
    this.flash.boot += 1;
    this.saveFlash();
    this.say(`Boot #${this.flash.boot}. ${Object.keys(this.flash.cards).length} cards in flash, ${this.flash.queue.length} events queued.`);
    this.connectMqtt();
    this.every(this.heartbeatMs, () => this.publishStatus());
    this.every(10_000, () => this.syncQueue());
    this.every(3_000, () => { if (this.mqttOnline) this.client.publish(T.CACHE_POLL, String(this.flash.cacheVersion)); });
    this.refreshLcd();
    this.changed();
  }

  async stop() {
    if (!this.running) return;
    this.running = false;
    for (const t of this.timers) { clearTimeout(t); clearInterval(t); }
    this.timers.clear();
    for (const t of this.pending.values()) clearTimeout(t.timer);
    if (this.client) {
      const client = this.client;
      this.client = null;
      if (client.connected) await client.publishAsync(T.ONLINE, 'offline', { retain: true, qos: 1 }).catch(() => {});
      await client.endAsync(true).catch(() => {});
    }
    this.mqttOnline = false;
    this.say('Powered off.');
    this.changed();
  }

  every(ms, fn) { const t = setInterval(() => this.running && fn(), ms); t.unref(); this.timers.add(t); return t; }
  after(ms, fn) {
    const t = setTimeout(() => { this.timers.delete(t); if (this.running) fn(); }, ms);
    this.timers.add(t);
    return t;
  }

  connectMqtt() {
    this.client = mqtt.connect(this.url, {
      clientId: 'parking-simulator',
      reconnectPeriod: 1500,
      connectTimeout: 3000,
      keepalive: 15,
      will: { topic: T.ONLINE, payload: 'offline', retain: true, qos: 1 },
    });
    const client = this.client;
    client.on('connect', () => {
      if (client !== this.client) return;
      this.mqttOnline = true;
      this.say('MQTT connected. System ONLINE.');
      client.subscribe([T.ACK, T.TIME, T.CARDS_VERSION, T.CACHE_UPDATE, T.OCCUPIED_RESPONSE, T.CMD], { qos: 1 });
      client.publish(T.ONLINE, 'online', { retain: true, qos: 1 });
      if (this.flash.occupied === null) client.publish(T.OCCUPIED_REQUEST, 'pull');
      this.publishStatus();
      this.after(300, () => this.syncQueue());
      this.refreshLcd();
      this.changed();
    });
    client.on('close', () => {
      if (client !== this.client || !this.mqttOnline) return;
      this.mqttOnline = false;
      this.say('MQTT link lost. Running standalone, events go to the flash queue.');
      this.refreshLcd();
      this.changed();
    });
    client.on('error', () => {});
    client.on('message', (topic, payload) => {
      if (client === this.client && this.running) this.onMessage(topic, payload.toString('utf8'));
    });
  }

  /** Simulates pulling the network cable / losing Wi-Fi (true = connected). */
  setNetwork(online) {
    if (!this.running || this.network === online) return;
    this.network = online;
    if (online) {
      this.say('Network restored. Reconnecting...');
      this.connectMqtt();
    } else {
      this.say('Network cut.');
      const client = this.client;
      this.client = null;
      this.mqttOnline = false;
      // Destroying the socket (instead of a clean disconnect) makes the broker
      // publish the last-will "offline" message, as with a real outage.
      client.options.reconnectPeriod = 0;
      client.stream?.destroy();
      client.end(true);
      this.refreshLcd();
    }
    this.changed();
  }

  // ---- messages from the backend ---------------------------------------
  onMessage(topic, text) {
    if (topic === T.ACK) return this.onAck(text.trim());
    if (topic === T.TIME) {
      const epochMs = Number(text) * 1000;
      if (epochMs > 0) this.timeOffset = epochMs - this.uptime();
      return undefined;
    }
    if (topic === T.CACHE_UPDATE) return this.onWhitelist(text);
    if (topic === T.OCCUPIED_RESPONSE) {
      const n = Number.parseInt(text, 10);
      if (this.flash.occupied === null && n >= 0 && n <= this.flash.total) {
        this.flash.occupied = n;
        this.saveFlash();
        this.say(`Occupancy restored from backend: ${n}.`);
        this.publishStatus();
        this.refreshLcd();
        this.changed();
      }
      return undefined;
    }
    if (topic === T.CMD) {
      let c;
      try { c = JSON.parse(text); } catch { return undefined; }
      return this.onCommand(c);
    }
    return undefined;
  }

  onWhitelist(text) {
    const lines = text.split('\n').map((l) => l.trim()).filter(Boolean);
    const head = /^v=(\d+)$/.exec(lines[0] || '');
    if (!head) return;
    const version = Number(head[1]);
    if (version === this.flash.cacheVersion) return;
    const cards = {};
    for (const line of lines.slice(1)) {
      const sep = line.indexOf('|');
      if (sep > 0) cards[line.slice(0, sep).toUpperCase()] = line.slice(sep + 1);
    }
    this.flash.cards = cards;
    this.flash.cacheVersion = version;
    this.saveFlash();
    this.say(`Whitelist v${version} stored (${Object.keys(cards).length} cards).`);
    this.publishStatus();
    this.refreshLcd();
    this.changed();
  }

  onCommand({ cmd, value }) {
    if (cmd === 'open_entry') this.gateCycle('entry', null);
    else if (cmd === 'open_exit') this.gateCycle('exit', null);
    else if (cmd === 'set_occupied' && Number.isInteger(value)) {
      this.flash.occupied = Math.min(Math.max(value, 0), this.flash.total);
      this.say(`Occupancy set to ${this.flash.occupied} by admin.`);
    } else if (cmd === 'set_total' && Number.isInteger(value) && value >= 1 && value <= 500) {
      this.flash.total = value;
      this.flash.occupied = Math.min(this.occupied, value);
      this.say(`Capacity set to ${value} by admin.`);
    } else if (cmd === 'sync') this.syncQueue();
    else return;
    this.saveFlash();
    this.publishStatus();
    this.refreshLcd();
    this.changed();
  }

  // ---- helpers ---------------------------------------------------------
  get occupied() { return this.flash.occupied ?? 0; }
  get cacheReady() { return this.flash.cacheVersion >= 0; }
  uptime() { return Date.now() - this.bootAt; }

  say(text) {
    this.log.push({ t: Date.now(), text });
    if (this.log.length > 60) this.log.shift();
  }

  beep(kind) { this.buzzer = { kind, at: Date.now() }; }

  publishStatus() {
    if (!this.mqttOnline) return;
    this.client.publish(T.STATUS, JSON.stringify({
      device: DEVICE,
      occupied: this.occupied,
      total: this.flash.total,
      free: this.flash.total - this.occupied,
      entryGate: this.gates.entry,
      exitGate: this.gates.exit,
      queued: this.flash.queue.length,
      cacheVersion: this.flash.cacheVersion,
      ip: '127.0.0.1',
      rssi: -42,
      uptime: Math.floor(this.uptime() / 1000),
    }), { retain: true, qos: 1 });
  }

  // ---- display ---------------------------------------------------------
  setLcd(gate, line1, line2, holdMs = 0) {
    this.lcd[gate] = [String(line1).slice(0, 16), String(line2).slice(0, 16)];
    if (holdMs) {
      clearTimeout(this.lcdHold[gate]);
      this.lcdHold[gate] = this.after(holdMs, () => { this.lcdHold[gate] = null; this.refreshLcd(); this.changed(); });
    }
  }

  refreshLcd() {
    const free = `Free: ${this.flash.total - this.occupied}/${this.flash.total}`;
    const mark = (text) => (this.mqttOnline ? text : `${text.padEnd(15)}*`); // * = offline
    for (const gate of ['entry', 'exit']) {
      if (this.busy[gate] || this.lcdHold[gate]) continue;
      if (!this.cacheReady) { this.lcd[gate] = ['System Init', 'Waiting sync...']; continue; }
      const waiting = this.sensors[`${gate}Before`];
      if (gate === 'entry') {
        if (this.occupied >= this.flash.total) this.lcd.entry = [mark('Parking FULL'), free];
        else this.lcd.entry = [mark('Welcome!'), waiting ? 'Scan Your Card' : free];
      } else {
        this.lcd.exit = [mark(waiting ? 'Goodbye!' : 'Exit Ready'), waiting ? 'Scan Your Card' : free];
      }
    }
  }

  // ---- sensors and cards (what a tester triggers) -----------------------
  setSensor(name, present) {
    if (!this.running || !(name in this.sensors)) return;
    this.sensors[name] = Boolean(present);
    const m = /^(entry|exit)After$/.exec(name);
    if (m) this.onAfterSensor(m[1]);
    this.refreshLcd();
    this.changed();
  }

  scan(gate, uidRaw) {
    if (!this.running) return { ok: false, message: 'The simulator is powered off.' };
    if (gate !== 'entry' && gate !== 'exit') return { ok: false, message: 'Unknown gate.' };
    if (this.busy[gate]) return { ok: false, message: 'Gate is busy with another vehicle.' };
    const uid = String(uidRaw).toUpperCase();
    this.say(`[${gate.toUpperCase()}] Card ${uid} scanned.`);

    let reason = null;
    if (!this.cacheReady) reason = 'cache_not_ready';
    else if (!(uid in this.flash.cards)) reason = 'unknown_card';
    else if (gate === 'entry' && this.occupied >= this.flash.total) reason = 'lot_full';

    if (reason) {
      this.beep('error');
      if (reason === 'lot_full') this.setLcd(gate, 'Parking FULL', 'Please wait', 2000);
      else this.setLcd(gate, 'INVALID CARD', 'Access Denied!', 2000);
      this.say(`[${gate.toUpperCase()}] Denied: ${reason}.`);
      this.logEvent('DENIED', { gate, uid, owner: this.flash.cards[uid] || '', reason });
      this.changed();
      return { ok: true, granted: false, reason };
    }
    this.gateCycle(gate, uid);
    return { ok: true, granted: true };
  }

  /** Opens a gate, waits for the vehicle to clear the "after" sensor, then closes. */
  gateCycle(gate, uid) {
    if (this.busy[gate]) return;
    const owner = uid ? this.flash.cards[uid] : '';
    const cycle = { uid, owner, seen: false, timer: null };
    this.busy[gate] = cycle;
    clearTimeout(this.lcdHold[gate]);
    this.lcdHold[gate] = null;
    this.beep(uid ? 'success' : 'open');
    this.gates[gate] = 'open';
    if (uid) this.setLcd(gate, `${gate === 'entry' ? 'Welcome' : 'Goodbye'} ${owner}`, 'Gate Opening...');
    else this.setLcd(gate, 'Remote open', 'Gate Opening...');
    this.say(`[${gate.toUpperCase()}] Gate open${uid ? ` for ${owner}` : ' (remote command)'}.`);
    this.publishStatus();
    cycle.timer = this.after(this.gateTimeoutMs, () => {
      // Timeout: only close if nothing is standing under the barrier.
      if (!cycle.seen) this.finishCycle(gate, false);
    });
    if (this.sensors[`${gate}After`]) this.onAfterSensor(gate);
    this.changed();
  }

  onAfterSensor(gate) {
    const cycle = this.busy[gate];
    if (!cycle) return;
    if (this.sensors[`${gate}After`]) {
      cycle.seen = true;
      this.setLcd(gate, 'Vehicle Detected', 'Please Proceed');
    } else if (cycle.seen) {
      this.finishCycle(gate, true); // vehicle has physically cleared the gate
    }
  }

  finishCycle(gate, passed) {
    const cycle = this.busy[gate];
    if (!cycle) return;
    clearTimeout(cycle.timer);
    if (passed) {
      const next = this.occupied + (gate === 'entry' ? 1 : -1);
      this.flash.occupied = Math.min(Math.max(next, 0), this.flash.total);
      this.saveFlash();
    }
    this.gates[gate] = 'closed';
    this.busy[gate] = null;
    this.say(`[${gate.toUpperCase()}] Gate closed. ${passed ? `Occupancy ${this.occupied}/${this.flash.total}.` : 'No vehicle passed.'}`);
    if (cycle.uid) {
      this.logEvent(gate === 'entry' ? 'ENTRY' : 'EXIT', { gate, uid: cycle.uid, owner: cycle.owner, passed });
    }
    this.publishStatus();
    this.refreshLcd();
    this.changed();
  }

  /** Plays a complete drive-through: arrive, scan, pass the barrier, leave. */
  drive(gate, uid, stepMs = 1200) {
    if (!this.running) return { ok: false, message: 'The simulator is powered off.' };
    if (this.busy[gate]) return { ok: false, message: 'Gate is busy with another vehicle.' };
    this.setSensor(`${gate}Before`, true);
    this.after(stepMs, () => {
      const result = this.scan(gate, uid);
      if (!result.granted) {
        this.after(stepMs * 2, () => this.setSensor(`${gate}Before`, false));
        return;
      }
      this.after(stepMs, () => {
        this.setSensor(`${gate}Before`, false);
        this.setSensor(`${gate}After`, true);
        this.after(stepMs, () => this.setSensor(`${gate}After`, false));
      });
    });
    return { ok: true };
  }

  // ---- store & forward ---------------------------------------------------
  logEvent(type, fields) {
    this.seq += 1;
    const evt = {
      id: `sim-${this.flash.boot}-${this.seq}`,
      type,
      device: DEVICE,
      ...fields,
      occupied: this.occupied,
      ts: this.timeOffset === null ? 0 : Math.floor((this.uptime() + this.timeOffset) / 1000),
      boot: this.flash.boot,
      up: this.uptime(),
      offline: false,
    };
    if (this.mqttOnline) {
      this.send(evt);
      // No ack in time -> keep a copy in flash and retry later.
      const timer = this.after(this.ackTimeoutMs, () => {
        this.pending.delete(evt.id);
        this.enqueue(evt);
      });
      this.pending.set(evt.id, { timer });
    } else {
      this.enqueue(evt);
    }
  }

  enqueue(evt) {
    this.flash.queue.push({ ...evt, offline: true });
    this.saveFlash();
    this.say(`[OFFLINE] ${evt.type} for ${evt.uid} saved to flash queue (${this.flash.queue.length} waiting).`);
    this.changed();
  }

  send(evt) {
    const topic = { ENTRY: T.ENTRY_LOG, EXIT: T.EXIT_LOG, DENIED: T.DENIED_LOG }[evt.type];
    const { type, boot, up, ...payload } = evt;
    // An event recorded before the clock was known can still be dated if it
    // happened during this boot: uptime-then + offset-now.
    if (!payload.ts && boot === this.flash.boot && this.timeOffset !== null) {
      payload.ts = Math.floor((up + this.timeOffset) / 1000);
    }
    this.client.publish(topic, JSON.stringify(payload), { qos: 1 });
  }

  syncQueue() {
    if (!this.mqttOnline || this.flash.queue.length === 0) return;
    this.say(`[SYNC] Sending ${this.flash.queue.length} buffered events...`);
    for (const evt of this.flash.queue) this.send(evt); // removed from flash when acked
    this.changed();
  }

  onAck(id) {
    const pending = this.pending.get(id);
    if (pending) { clearTimeout(pending.timer); this.timers.delete(pending.timer); this.pending.delete(id); }
    const before = this.flash.queue.length;
    this.flash.queue = this.flash.queue.filter((e) => e.id !== id);
    if (this.flash.queue.length !== before) {
      this.saveFlash();
      if (this.flash.queue.length === 0) this.say('[SYNC] Complete. Flash queue is empty.');
      this.publishStatus();
      this.changed();
    }
  }

  clearFlash() {
    this.flash = { boot: this.flash.boot, total: 10, occupied: null, cacheVersion: -1, cards: {}, queue: [] };
    this.saveFlash();
    this.say('Flash erased (whitelist, queue and counters).');
    this.refreshLcd();
    this.changed();
  }

  // ---- snapshot for the dashboard --------------------------------------
  getState() {
    return {
      running: this.running,
      network: this.network,
      mqttOnline: this.mqttOnline,
      total: this.flash.total,
      occupied: this.occupied,
      cacheVersion: this.flash.cacheVersion,
      cards: Object.entries(this.flash.cards).map(([uid, owner]) => ({ uid, owner })),
      queue: this.flash.queue.map(({ id, type, uid, owner, gate, reason }) => ({ id, type, uid, owner, gate, reason })),
      sensors: this.sensors,
      sensorDistanceCm: Object.fromEntries(Object.entries(this.sensors).map(([k, v]) => [k, v ? CAR_NEAR_CM - 7 : 200])),
      gates: this.gates,
      busy: { entry: Boolean(this.busy.entry), exit: Boolean(this.busy.exit) },
      lcd: this.lcd,
      buzzer: this.buzzer,
      clockSynced: this.timeOffset !== null,
      log: this.log.slice(-30),
    };
  }

  changed() { this.emit('change', this.getState()); }
}

module.exports = { VirtualDevice };
