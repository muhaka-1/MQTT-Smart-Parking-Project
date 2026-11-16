'use strict';
// SQLite persistence (uses Node's built-in node:sqlite, no native build step).
const fs = require('node:fs');
const path = require('node:path');
const { DatabaseSync } = require('node:sqlite');

const SCHEMA = `
CREATE TABLE IF NOT EXISTS cards (
  uid        TEXT PRIMARY KEY,            -- "A1:B2:C3:D4"
  owner      TEXT NOT NULL,
  plate      TEXT NOT NULL DEFAULT '',
  status     TEXT NOT NULL DEFAULT 'active' CHECK (status IN ('active','blocked')),
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS events (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  event_id     TEXT UNIQUE,               -- id generated on the device (dedupe key)
  type         TEXT NOT NULL,             -- ENTRY | EXIT | DENIED | ADMIN
  gate         TEXT NOT NULL DEFAULT '',  -- entry | exit
  uid          TEXT NOT NULL DEFAULT '',
  owner        TEXT NOT NULL DEFAULT '',
  reason       TEXT NOT NULL DEFAULT '',
  passed       INTEGER NOT NULL DEFAULT 0,-- 1 = vehicle actually drove through
  ts           INTEGER NOT NULL,          -- when it happened (ms since epoch)
  ts_estimated INTEGER NOT NULL DEFAULT 0,-- 1 = device had no clock, ts = arrival time
  received_at  INTEGER NOT NULL,
  offline      INTEGER NOT NULL DEFAULT 0,-- 1 = was buffered on the device first
  occupied     INTEGER,                   -- occupancy right after the event
  device       TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_events_ts  ON events(ts);
CREATE INDEX IF NOT EXISTS idx_events_uid ON events(uid);
CREATE TABLE IF NOT EXISTS occupancy (
  ts       INTEGER NOT NULL,
  occupied INTEGER NOT NULL,
  total    INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_occupancy_ts ON occupancy(ts);
CREATE TABLE IF NOT EXISTS settings (
  key   TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
`;

const DEMO_CARDS = [
  ['A1:B2:C3:D4', 'Jubayer Alam', 'ABC 123', 'active'],
  ['11:22:33:44', 'Sara Lindqvist', 'KLM 456', 'active'],
  ['DE:AD:BE:EF', 'Omar Hassan', 'XYZ 789', 'active'],
  ['0A:1B:2C:3D', 'Guest card 1', '', 'blocked'],
];

/** Accepts "a1b2c3d4", "A1 B2 C3 D4", "a1:b2:c3:d4" ... returns "A1:B2:C3:D4" or null. */
function normalizeUid(input) {
  const hex = String(input ?? '').replace(/[^0-9a-fA-F]/g, '').toUpperCase();
  if (hex.length < 8 || hex.length > 20 || hex.length % 2) return null;
  return hex.match(/../g).join(':');
}

/** Names travel to the device as "UID|Owner" lines, so strip the separators. */
function cleanText(input, max = 40) {
  return String(input ?? '').replace(/[|\r\n\t]/g, ' ').replace(/\s+/g, ' ').trim().slice(0, max);
}

function open(dataDir, { seedDemoData = true } = {}) {
  fs.mkdirSync(dataDir, { recursive: true });
  const file = path.join(dataDir, 'parking.db');
  const isNew = !fs.existsSync(file);
  const sql = new DatabaseSync(file);
  sql.exec('PRAGMA journal_mode = WAL; PRAGMA foreign_keys = ON;');
  sql.exec(SCHEMA);

  const q = {
    getSetting: sql.prepare('SELECT value FROM settings WHERE key = ?'),
    setSetting: sql.prepare(
      'INSERT INTO settings(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value'),
    cards: sql.prepare('SELECT * FROM cards ORDER BY owner COLLATE NOCASE'),
    card: sql.prepare('SELECT * FROM cards WHERE uid = ?'),
    insertCard: sql.prepare(
      'INSERT INTO cards(uid, owner, plate, status, created_at, updated_at) VALUES(?,?,?,?,?,?)'),
    updateCard: sql.prepare('UPDATE cards SET owner = ?, plate = ?, status = ?, updated_at = ? WHERE uid = ?'),
    deleteCard: sql.prepare('DELETE FROM cards WHERE uid = ?'),
    insertEvent: sql.prepare(`INSERT OR IGNORE INTO events
      (event_id, type, gate, uid, owner, reason, passed, ts, ts_estimated, received_at, offline, occupied, device)
      VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)`),
    eventById: sql.prepare('SELECT * FROM events WHERE id = ?'),
    insertOccupancy: sql.prepare('INSERT INTO occupancy(ts, occupied, total) VALUES(?,?,?)'),
    lastOccupancy: sql.prepare('SELECT * FROM occupancy ORDER BY ts DESC, rowid DESC LIMIT 1'),
  };

  const db = {
    file,
    close: () => sql.close(),

    getSetting(key, fallback = null) {
      const row = q.getSetting.get(key);
      return row ? row.value : fallback;
    },
    getNumber(key, fallback) {
      const value = db.getSetting(key);
      const n = Number(value);
      return value === null || !Number.isFinite(n) ? fallback : n;
    },
    setSetting: (key, value) => { q.setSetting.run(key, String(value)); },

    // ---- cards -----------------------------------------------------------
    listCards() {
      const stats = new Map(sql.prepare(`
        SELECT uid,
               SUM(type = 'ENTRY' AND passed = 1) AS entries,
               SUM(type = 'EXIT'  AND passed = 1) AS exits,
               MAX(ts) AS last_seen
        FROM events WHERE uid != '' GROUP BY uid`).all().map((r) => [r.uid, r]));
      const inside = new Set(db.cardsInside().map((r) => r.uid));
      return q.cards.all().map((c) => ({
        ...c,
        entries: stats.get(c.uid)?.entries ?? 0,
        exits: stats.get(c.uid)?.exits ?? 0,
        last_seen: stats.get(c.uid)?.last_seen ?? null,
        inside: inside.has(c.uid),
      }));
    },
    getCard: (uid) => q.card.get(uid),
    activeCards: () => q.cards.all().filter((c) => c.status === 'active'),
    createCard({ uid, owner, plate = '', status = 'active' }) {
      const now = Date.now();
      q.insertCard.run(uid, owner, plate, status, now, now);
      return q.card.get(uid);
    },
    updateCard(uid, { owner, plate, status }) {
      const cur = q.card.get(uid);
      if (!cur) return null;
      q.updateCard.run(owner ?? cur.owner, plate ?? cur.plate, status ?? cur.status, Date.now(), uid);
      return q.card.get(uid);
    },
    deleteCard: (uid) => q.deleteCard.run(uid).changes > 0,

    // ---- events ----------------------------------------------------------
    /** Returns the stored row, or null when event_id was already stored (duplicate). */
    addEvent(e) {
      const res = q.insertEvent.run(
        e.event_id ?? null, e.type, e.gate ?? '', e.uid ?? '', e.owner ?? '', e.reason ?? '',
        e.passed ? 1 : 0, e.ts, e.ts_estimated ? 1 : 0, e.received_at ?? Date.now(),
        e.offline ? 1 : 0, e.occupied ?? null, e.device ?? '');
      return res.changes ? q.eventById.get(res.lastInsertRowid) : null;
    },
    listEvents({ type, uid, search, from, to, limit = 100, offset = 0 } = {}) {
      const where = [];
      const args = [];
      if (type) { where.push('type = ?'); args.push(type); }
      if (uid) { where.push('uid = ?'); args.push(uid); }
      if (search) {
        where.push('(uid LIKE ? OR owner LIKE ? OR reason LIKE ?)');
        const s = `%${search}%`;
        args.push(s, s, s);
      }
      if (from) { where.push('ts >= ?'); args.push(Number(from)); }
      if (to) { where.push('ts <= ?'); args.push(Number(to)); }
      const w = where.length ? `WHERE ${where.join(' AND ')}` : '';
      const total = sql.prepare(`SELECT COUNT(*) AS n FROM events ${w}`).get(...args).n;
      const rows = sql.prepare(`SELECT * FROM events ${w} ORDER BY ts DESC, id DESC LIMIT ? OFFSET ?`)
        .all(...args, Math.min(Number(limit) || 100, 5000), Number(offset) || 0);
      return { total, rows };
    },
    clearEvents() {
      sql.exec('DELETE FROM events; DELETE FROM occupancy;');
    },
    /** Cards whose most recent completed passage was an entry. */
    cardsInside() {
      return sql.prepare(`
        SELECT e.uid, e.owner, e.ts AS since, c.plate
        FROM events e
        LEFT JOIN cards c ON c.uid = e.uid
        WHERE e.passed = 1 AND e.type IN ('ENTRY','EXIT')
          AND e.id = (SELECT id FROM events x
                      WHERE x.uid = e.uid AND x.passed = 1 AND x.type IN ('ENTRY','EXIT')
                      ORDER BY x.ts DESC, x.id DESC LIMIT 1)
          AND e.type = 'ENTRY'
        ORDER BY e.ts DESC`).all();
    },

    // ---- occupancy + statistics -----------------------------------------
    recordOccupancy(occupied, total, ts = Date.now()) {
      const last = q.lastOccupancy.get();
      if (last && last.occupied === occupied && last.total === total) return false;
      q.insertOccupancy.run(ts, occupied, total);
      return true;
    },
    stats({ from, to = Date.now() }) {
      const counts = sql.prepare(`
        SELECT COALESCE(SUM(type = 'ENTRY' AND passed = 1), 0) AS entries,
               COALESCE(SUM(type = 'EXIT'  AND passed = 1), 0) AS exits,
               COALESCE(SUM(type = 'DENIED'), 0)               AS denied,
               COALESCE(SUM(offline = 1), 0)                   AS buffered
        FROM events WHERE ts >= ? AND ts <= ?`).get(from, to);
      const before = sql.prepare('SELECT * FROM occupancy WHERE ts < ? ORDER BY ts DESC, rowid DESC LIMIT 1').get(from);
      const series = sql.prepare('SELECT ts, occupied, total FROM occupancy WHERE ts >= ? AND ts <= ? ORDER BY ts, rowid')
        .all(from, to);
      if (before) series.unshift({ ts: from, occupied: before.occupied, total: before.total });
      const passages = sql.prepare(`
        SELECT ts, type FROM events
        WHERE passed = 1 AND type IN ('ENTRY','EXIT') AND ts >= ? AND ts <= ?`).all(from, to);
      const topCards = sql.prepare(`
        SELECT uid, MAX(owner) AS owner, COUNT(*) AS visits FROM events
        WHERE type = 'ENTRY' AND passed = 1 AND ts >= ? AND ts <= ?
        GROUP BY uid ORDER BY visits DESC LIMIT 5`).all(from, to);
      const deniedReasons = sql.prepare(`
        SELECT reason, COUNT(*) AS n FROM events
        WHERE type = 'DENIED' AND ts >= ? AND ts <= ? GROUP BY reason ORDER BY n DESC`).all(from, to);
      return { from, to, counts, occupancy: series, passages, topCards, deniedReasons, inside: db.cardsInside() };
    },
  };

  if (isNew) {
    db.setSetting('cards_version', 1);
    db.setSetting('total_spaces', 10);
    db.setSetting('occupied', 0);
    if (seedDemoData) {
      for (const [uid, owner, plate, status] of DEMO_CARDS) db.createCard({ uid, owner, plate, status });
    }
  }
  return db;
}

module.exports = { open, normalizeUid, cleanText };
