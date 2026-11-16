'use strict';
// HTTP side: REST API, live updates (Server-Sent Events) and the dashboard files.
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { normalizeUid, cleanText } = require('./db');

const PUBLIC_DIR = path.join(__dirname, '..', 'public');
const MIME = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8', '.svg': 'image/svg+xml', '.png': 'image/png', '.ico': 'image/x-icon',
};
const SESSION_MS = 12 * 60 * 60 * 1000;

class HttpError extends Error {
  constructor(status, message) { super(message); this.status = status; }
}

function createServer({ config, db, parking, sim }) {
  const sessions = new Map();   // token -> expiry
  const streams = new Set();    // open SSE responses
  let failedLogins = 0;

  // ---- live updates ------------------------------------------------------
  function broadcast(name, data) {
    const frame = `event: ${name}\ndata: ${JSON.stringify(data ?? {})}\n\n`;
    for (const res of streams) res.write(frame);
  }
  parking.on('state', (s) => broadcast('state', s));
  parking.on('event', (e) => broadcast('activity', e));
  parking.on('cards', () => broadcast('cards'));
  if (sim) sim.on('change', (s) => broadcast('sim', s));
  const keepAlive = setInterval(() => { for (const res of streams) res.write(': ping\n\n'); }, 25_000);
  keepAlive.unref();

  // ---- helpers -----------------------------------------------------------
  const send = (res, status, body, headers = {}) => {
    const data = typeof body === 'string' || Buffer.isBuffer(body) ? body : JSON.stringify(body);
    res.writeHead(status, {
      'Content-Type': typeof body === 'string' || Buffer.isBuffer(body) ? 'text/plain; charset=utf-8' : 'application/json',
      'Cache-Control': 'no-store',
      ...headers,
    });
    res.end(data);
  };

  function readJson(req) {
    return new Promise((resolve, reject) => {
      let size = 0;
      const chunks = [];
      req.on('data', (c) => {
        size += c.length;
        if (size > 64 * 1024) { reject(new HttpError(413, 'Request is too large.')); req.destroy(); return; }
        chunks.push(c);
      });
      req.on('end', () => {
        if (!chunks.length) return resolve({});
        try { resolve(JSON.parse(Buffer.concat(chunks).toString('utf8'))); } catch { reject(new HttpError(400, 'Body must be JSON.')); }
        return undefined;
      });
      req.on('error', reject);
    });
  }

  function sessionOf(req) {
    const m = /(?:^|;\s*)sid=([a-f0-9]{48})/.exec(req.headers.cookie || '');
    if (!m) return null;
    const expiry = sessions.get(m[1]);
    if (!expiry || expiry < Date.now()) { sessions.delete(m[1]); return null; }
    return m[1];
  }

  const sameSecret = (a, b) => crypto.timingSafeEqual(
    crypto.createHash('sha256').update(String(a)).digest(),
    crypto.createHash('sha256').update(String(b)).digest());

  function requireUid(raw) {
    const uid = normalizeUid(raw);
    if (!uid) throw new HttpError(400, 'Card UID must be 4 to 10 hex bytes, for example A1:B2:C3:D4.');
    return uid;
  }

  function requireSim() {
    if (!sim) throw new HttpError(404, 'The simulator is disabled (SIMULATOR=off).');
    return sim;
  }

  const gateOf = (g) => {
    if (g !== 'entry' && g !== 'exit') throw new HttpError(400, 'Gate must be "entry" or "exit".');
    return g;
  };

  function csv(rows) {
    const cell = (v) => {
      let s = v === null || v === undefined ? '' : String(v);
      if (/^[=+\-@\t\r]/.test(s)) s = `'${s}`; // keep spreadsheets from running it as a formula
      return /[",\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
    };
    const head = ['time', 'type', 'gate', 'uid', 'owner', 'result', 'reason', 'buffered_offline', 'occupied_after', 'device'];
    const lines = rows.map((r) => [
      new Date(r.ts).toISOString(), r.type, r.gate, r.uid, r.owner,
      r.type === 'DENIED' ? 'denied' : r.type === 'ADMIN' ? 'admin' : r.passed ? 'passed' : 'no vehicle passed',
      r.reason, r.offline ? 'yes' : 'no', r.occupied, r.device,
    ].map(cell).join(','));
    return `${head.join(',')}\n${lines.join('\n')}\n`;
  }

  // ---- API routes ----------------------------------------------------------
  const routes = [];
  const route = (method, pattern, handler, { open = false } = {}) => {
    const keys = [];
    const re = new RegExp(`^${pattern.replace(/:(\w+)/g, (_, k) => { keys.push(k); return '([^/]+)'; })}$`);
    routes.push({ method, re, keys, handler, open });
  };

  route('POST', '/api/login', async ({ req, res }) => {
    const { password } = await readJson(req);
    await new Promise((r) => { setTimeout(r, Math.min(failedLogins * 300, 3000)); }); // slow down guessing
    if (!sameSecret(password ?? '', config.adminPassword)) {
      failedLogins += 1;
      throw new HttpError(401, 'Wrong password.');
    }
    failedLogins = 0;
    const token = crypto.randomBytes(24).toString('hex');
    sessions.set(token, Date.now() + SESSION_MS);
    send(res, 200, { ok: true }, { 'Set-Cookie': `sid=${token}; HttpOnly; SameSite=Strict; Path=/; Max-Age=${SESSION_MS / 1000}` });
  }, { open: true });

  route('POST', '/api/logout', ({ req, res }) => {
    sessions.delete(sessionOf(req));
    send(res, 200, { ok: true }, { 'Set-Cookie': 'sid=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0' });
  });

  route('GET', '/api/session', ({ req, res }) => send(res, 200, {
    authenticated: Boolean(sessionOf(req)),
    defaultPassword: config.adminPassword === 'admin',
  }), { open: true });

  route('GET', '/api/state', ({ res }) => send(res, 200, {
    ...parking.getState(), simulatorAvailable: Boolean(sim), embeddedBroker: !config.mqttUrl, mqttPort: config.mqttPort,
  }));

  route('GET', '/api/stream', ({ req, res }) => {
    res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-store', Connection: 'keep-alive', 'X-Accel-Buffering': 'no' });
    res.write(`event: state\ndata: ${JSON.stringify(parking.getState())}\n\n`);
    if (sim) res.write(`event: sim\ndata: ${JSON.stringify(sim.getState())}\n\n`);
    streams.add(res);
    req.on('close', () => streams.delete(res));
  });

  // cards
  route('GET', '/api/cards', ({ res }) => send(res, 200, db.listCards()));
  route('POST', '/api/cards', async ({ req, res }) => {
    const body = await readJson(req);
    const uid = requireUid(body.uid);
    const owner = cleanText(body.owner);
    if (!owner) throw new HttpError(400, 'Enter the name of the card holder.');
    if (db.getCard(uid)) throw new HttpError(409, `Card ${uid} is already registered.`);
    const status = body.status === 'blocked' ? 'blocked' : 'active';
    const card = db.createCard({ uid, owner, plate: cleanText(body.plate, 16), status });
    parking.cardsChanged();
    send(res, 201, card);
  });
  route('PUT', '/api/cards/:uid', async ({ req, res, params }) => {
    const body = await readJson(req);
    const uid = requireUid(params.uid);
    const patch = {};
    if (body.owner !== undefined) {
      patch.owner = cleanText(body.owner);
      if (!patch.owner) throw new HttpError(400, 'Enter the name of the card holder.');
    }
    if (body.plate !== undefined) patch.plate = cleanText(body.plate, 16);
    if (body.status !== undefined) {
      if (!['active', 'blocked'].includes(body.status)) throw new HttpError(400, 'Status must be "active" or "blocked".');
      patch.status = body.status;
    }
    const card = db.updateCard(uid, patch);
    if (!card) throw new HttpError(404, `Card ${uid} is not registered.`);
    parking.cardsChanged();
    send(res, 200, card);
  });
  route('DELETE', '/api/cards/:uid', ({ res, params }) => {
    const uid = requireUid(params.uid);
    if (!db.deleteCard(uid)) throw new HttpError(404, `Card ${uid} is not registered.`);
    parking.cardsChanged();
    send(res, 200, { ok: true });
  });

  // activity + statistics
  const eventFilter = (query) => ({
    type: ['ENTRY', 'EXIT', 'DENIED', 'ADMIN'].includes(query.get('type')) ? query.get('type') : undefined,
    search: query.get('search') || undefined,
    from: query.get('from') || undefined,
    to: query.get('to') || undefined,
    limit: query.get('limit') || 100,
    offset: query.get('offset') || 0,
  });
  route('GET', '/api/events', ({ res, query }) => send(res, 200, db.listEvents(eventFilter(query))));
  route('GET', '/api/events.csv', ({ res, query }) => {
    const { rows } = db.listEvents({ ...eventFilter(query), limit: 5000, offset: 0 });
    send(res, 200, csv(rows), {
      'Content-Type': 'text/csv; charset=utf-8',
      'Content-Disposition': `attachment; filename="parking-activity-${new Date().toISOString().slice(0, 10)}.csv"`,
    });
  });
  route('DELETE', '/api/events', ({ res }) => {
    db.clearEvents();
    broadcast('activity', null);
    send(res, 200, { ok: true });
  });
  route('GET', '/api/stats', ({ res, query }) => {
    const to = Date.now();
    const from = Number(query.get('from')) || to - 24 * 60 * 60 * 1000;
    send(res, 200, db.stats({ from, to }));
  });

  // commands to the gate controller
  route('POST', '/api/gates/:gate/open', ({ res, params }) => {
    const gate = gateOf(params.gate);
    parking.sendCommand(`open_${gate}`, undefined, `Remote open of ${gate} gate`);
    send(res, 200, { ok: true });
  });
  route('POST', '/api/settings', async ({ req, res }) => {
    const body = await readJson(req);
    const state = parking.getState();
    let total = state.total;
    if (body.total !== undefined) {
      total = Number(body.total);
      if (!Number.isInteger(total) || total < 1 || total > 500) throw new HttpError(400, 'Capacity must be a whole number from 1 to 500.');
      parking.sendCommand('set_total', total, `Capacity set to ${total}`);
    }
    if (body.occupied !== undefined) {
      const occupied = Number(body.occupied);
      if (!Number.isInteger(occupied) || occupied < 0 || occupied > total) {
        throw new HttpError(400, `Occupied spaces must be a whole number from 0 to ${total}.`);
      }
      parking.sendCommand('set_occupied', occupied, `Occupancy corrected to ${occupied}`);
    }
    send(res, 200, { ok: true });
  });
  route('POST', '/api/whitelist/push', ({ res }) => {
    parking.publishWhitelist();
    send(res, 200, { ok: true });
  });

  // simulator (test bench)
  route('GET', '/api/sim', ({ res }) => send(res, 200, requireSim().getState()));
  route('POST', '/api/sim/power', async ({ req, res }) => {
    const { on } = await readJson(req);
    if (on) requireSim().start(); else await requireSim().stop();
    send(res, 200, requireSim().getState());
  });
  route('POST', '/api/sim/network', async ({ req, res }) => {
    const { online } = await readJson(req);
    requireSim().setNetwork(Boolean(online));
    send(res, 200, requireSim().getState());
  });
  route('POST', '/api/sim/sensor', async ({ req, res }) => {
    const { name, present } = await readJson(req);
    if (!(name in requireSim().sensors)) throw new HttpError(400, 'Unknown sensor.');
    sim.setSensor(name, Boolean(present));
    send(res, 200, sim.getState());
  });
  route('POST', '/api/sim/scan', async ({ req, res }) => {
    const body = await readJson(req);
    const result = requireSim().scan(gateOf(body.gate), requireUid(body.uid));
    if (!result.ok) throw new HttpError(409, result.message);
    send(res, 200, result);
  });
  route('POST', '/api/sim/drive', async ({ req, res }) => {
    const body = await readJson(req);
    const result = requireSim().drive(gateOf(body.gate), requireUid(body.uid));
    if (!result.ok) throw new HttpError(409, result.message);
    send(res, 200, result);
  });
  route('POST', '/api/sim/erase', ({ res }) => {
    requireSim().clearFlash();
    send(res, 200, sim.getState());
  });

  // ---- request handling ------------------------------------------------------
  function serveStatic(reqPath, res) {
    const rel = reqPath === '/' ? 'index.html' : decodeURIComponent(reqPath.slice(1));
    const file = path.normalize(path.join(PUBLIC_DIR, rel));
    if (!file.startsWith(PUBLIC_DIR + path.sep)) return send(res, 403, 'Forbidden');
    return fs.readFile(file, (err, data) => {
      if (err) return send(res, 404, 'Not found');
      return send(res, 200, data, {
        'Content-Type': MIME[path.extname(file)] || 'application/octet-stream',
        'Content-Security-Policy': "default-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:",
        'X-Content-Type-Options': 'nosniff',
      });
    });
  }

  const server = http.createServer(async (req, res) => {
    const url = new URL(req.url, 'http://localhost');
    try {
      if (!url.pathname.startsWith('/api/')) {
        if (req.method !== 'GET') throw new HttpError(405, 'Method not allowed.');
        return serveStatic(url.pathname, res);
      }
      for (const r of routes) {
        const m = r.method === req.method && r.re.exec(url.pathname);
        if (!m) continue;
        if (!r.open && !sessionOf(req)) throw new HttpError(401, 'Sign in first.');
        const params = Object.fromEntries(r.keys.map((k, i) => [k, decodeURIComponent(m[i + 1])]));
        return await r.handler({ req, res, params, query: url.searchParams });
      }
      throw new HttpError(404, 'Unknown API endpoint.');
    } catch (err) {
      if (res.headersSent) return res.end();
      const status = err.status || 500;
      if (status === 500) console.error(err);
      return send(res, status, { error: status === 500 ? 'Internal server error.' : err.message });
    }
  });

  server.on('close', () => {
    clearInterval(keepAlive);
    for (const res of streams) res.end();
  });
  return server;
}

module.exports = { createServer };
