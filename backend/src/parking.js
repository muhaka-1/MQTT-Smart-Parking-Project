'use strict';
// The backend's MQTT side: stores what the gate controller reports, keeps the
// controller's card whitelist up to date and relays admin commands.
const { EventEmitter } = require('node:events');
const mqtt = require('mqtt');
const T = require('./topics');

const EVENT_TOPICS = { [T.ENTRY_LOG]: 'ENTRY', [T.EXIT_LOG]: 'EXIT', [T.DENIED_LOG]: 'DENIED' };
const WHITELIST_SOFT_LIMIT = 3800; // firmware MQTT buffer is 4096 bytes

class ParkingService extends EventEmitter {
  constructor(db) {
    super();
    this.db = db;
    this.client = null;
    this.brokerConnected = false;
    this.deviceOnline = false;
    this.deviceSeenAt = null;
    this.status = null;          // last parking/status payload
    this.devicesSeen = new Map(); // device name -> last status time
  }

  connect(url) {
    this.client = mqtt.connect(url, {
      clientId: `parking-backend-${Math.random().toString(16).slice(2, 8)}`,
      reconnectPeriod: 2000,
      connectTimeout: 5000,
    });
    this.client.on('connect', () => {
      this.brokerConnected = true;
      this.client.subscribe([
        T.ONLINE, T.STATUS, T.ENTRY_LOG, T.EXIT_LOG, T.DENIED_LOG, T.CACHE_POLL, T.OCCUPIED_REQUEST,
      ], { qos: 1 });
      this.publishWhitelist();
      this.publishTime();
      this.emitState();
    });
    this.client.on('close', () => {
      if (!this.brokerConnected) return;
      this.brokerConnected = false;
      this.emitState();
    });
    this.client.on('error', (err) => this.emit('log', `MQTT error: ${err.message}`));
    this.client.on('message', (topic, payload) => {
      try {
        this.onMessage(topic, payload.toString('utf8'));
      } catch (err) {
        this.emit('log', `Bad message on ${topic}: ${err.message}`);
      }
    });
    this.timeTimer = setInterval(() => this.publishTime(), 30_000);
    this.timeTimer.unref();
    return new Promise((resolve) => this.client.once('connect', resolve));
  }

  async close() {
    clearInterval(this.timeTimer);
    if (this.client) await this.client.endAsync(true);
  }

  // ---- incoming ----------------------------------------------------------
  onMessage(topic, text) {
    if (topic === T.ONLINE) {
      this.deviceOnline = text === 'online';
      if (this.deviceOnline) {
        this.deviceSeenAt = Date.now();
        this.publishTime();
      }
      return this.emitState();
    }
    if (topic === T.STATUS) return this.onStatus(JSON.parse(text));
    if (EVENT_TOPICS[topic]) return this.onDeviceEvent(EVENT_TOPICS[topic], JSON.parse(text));
    if (topic === T.CACHE_POLL) {
      if (Number(text) !== this.db.getNumber('cards_version', 1)) this.publishWhitelist();
      return undefined;
    }
    if (topic === T.OCCUPIED_REQUEST) {
      this.client.publish(T.OCCUPIED_RESPONSE, String(this.db.getNumber('occupied', 0)));
    }
    return undefined;
  }

  onStatus(s) {
    const occupied = Number(s.occupied);
    const total = Number(s.total);
    if (!Number.isInteger(occupied) || !Number.isInteger(total)) return;
    this.status = { ...s, occupied, total, free: Math.max(total - occupied, 0), receivedAt: Date.now() };
    this.deviceSeenAt = Date.now();
    this.devicesSeen.set(String(s.device || 'unknown'), Date.now());
    this.db.setSetting('occupied', occupied);
    this.db.setSetting('total_spaces', total);
    this.db.recordOccupancy(occupied, total);
    this.emitState();
  }

  onDeviceEvent(type, e) {
    const now = Date.now();
    const deviceTs = Number(e.ts) * 1000;
    // A device that booted without network has no clock: fall back to arrival time.
    const plausible = deviceTs > Date.UTC(2024, 0, 1) && deviceTs < now + 5 * 60_000;
    const uid = String(e.uid || '');
    const card = uid ? this.db.getCard(uid) : null;
    let reason = String(e.reason || '');
    if (type === 'DENIED' && reason === 'unknown_card' && card?.status === 'blocked') reason = 'blocked_card';

    const row = this.db.addEvent({
      event_id: e.id ? String(e.id) : null,
      type,
      gate: String(e.gate || (type === 'EXIT' ? 'exit' : 'entry')),
      uid,
      owner: card?.owner || String(e.owner || ''),
      reason,
      passed: type !== 'DENIED' && e.passed !== false && e.passed !== 0,
      ts: plausible ? deviceTs : now,
      ts_estimated: !plausible,
      received_at: now,
      offline: Boolean(e.offline),
      occupied: Number.isInteger(e.occupied) ? e.occupied : null,
      device: String(e.device || ''),
    });
    // Always acknowledge, also duplicates, so the device can drop its copy.
    if (e.id) this.client.publish(T.ACK, String(e.id), { qos: 1 });
    if (row) this.emit('event', row);
  }

  // ---- outgoing ----------------------------------------------------------
  publishTime() {
    if (this.brokerConnected) this.client.publish(T.TIME, String(Math.floor(Date.now() / 1000)));
  }

  /** Sends the list of active cards to the device (retained, so it also arrives after a reconnect). */
  publishWhitelist() {
    if (!this.brokerConnected) return;
    const version = this.db.getNumber('cards_version', 1);
    const lines = this.db.activeCards().map((c) => `${c.uid}|${c.owner}`);
    const payload = `v=${version}\n${lines.join('\n')}${lines.length ? '\n' : ''}`;
    if (Buffer.byteLength(payload) > WHITELIST_SOFT_LIMIT) {
      this.emit('log', `Whitelist is ${Buffer.byteLength(payload)} bytes; the ESP32 buffer is 4096. Raise MQTT_BUFFER_SIZE in the firmware.`);
    }
    this.client.publish(T.CACHE_UPDATE, payload, { qos: 1, retain: true });
    this.client.publish(T.CARDS_VERSION, String(version), { qos: 1, retain: true });
  }

  /** Call after any card change. */
  cardsChanged() {
    this.db.setSetting('cards_version', this.db.getNumber('cards_version', 1) + 1);
    this.publishWhitelist();
    this.emit('cards');
    this.emitState();
  }

  /** Sends a command to the gate controller. Throws if it cannot be delivered. */
  sendCommand(cmd, value, note) {
    if (!this.brokerConnected) throw Object.assign(new Error('The MQTT broker is not reachable.'), { status: 503 });
    if (!this.deviceOnline) {
      throw Object.assign(new Error('The gate controller is offline, so the command was not sent.'), { status: 409 });
    }
    this.client.publish(T.CMD, JSON.stringify(value === undefined ? { cmd } : { cmd, value }), { qos: 1 });
    const row = this.db.addEvent({
      type: 'ADMIN',
      gate: cmd === 'open_entry' ? 'entry' : cmd === 'open_exit' ? 'exit' : '',
      reason: note || cmd,
      ts: Date.now(),
      device: 'dashboard',
    });
    this.emit('event', row);
  }

  // ---- state for the dashboard --------------------------------------------
  getState() {
    const now = Date.now();
    const recentDevices = [...this.devicesSeen].filter(([, t]) => now - t < 90_000).map(([name]) => name);
    const total = this.status?.total ?? this.db.getNumber('total_spaces', 10);
    const occupied = this.status?.occupied ?? this.db.getNumber('occupied', 0);
    return {
      broker: this.brokerConnected,
      deviceOnline: this.deviceOnline,
      deviceSeenAt: this.deviceSeenAt,
      device: this.status?.device ?? null,
      conflictingDevices: recentDevices.length > 1 ? recentDevices : [],
      total,
      occupied,
      free: Math.max(total - occupied, 0),
      entryGate: this.deviceOnline ? this.status?.entryGate ?? 'closed' : 'unknown',
      exitGate: this.deviceOnline ? this.status?.exitGate ?? 'closed' : 'unknown',
      queued: this.status?.queued ?? 0,
      deviceCacheVersion: this.status?.cacheVersion ?? null,
      cardsVersion: this.db.getNumber('cards_version', 1),
      ip: this.status?.ip ?? null,
      rssi: this.status?.rssi ?? null,
      uptime: this.status?.uptime ?? null,
      statusAt: this.status?.receivedAt ?? null,
    };
  }

  emitState() {
    this.emit('state', this.getState());
  }
}

module.exports = { ParkingService };
