'use strict';
// Wires everything together: (optional) embedded MQTT broker, database,
// parking service, simulator and the HTTP server.
const net = require('node:net');
const { open: openDb } = require('./db');
const { ParkingService } = require('./parking');
const { VirtualDevice } = require('./simulator');
const { createServer } = require('./server');

function startEmbeddedBroker(port) {
  // eslint-disable-next-line global-require
  const aedes = require('aedes')();
  const tcp = net.createServer(aedes.handle);
  return new Promise((resolve, reject) => {
    tcp.once('error', (err) => {
      if (err.code === 'EADDRINUSE') {
        reject(new Error(
          `Port ${port} is already in use, probably by another MQTT broker (Mosquitto?).\n`
          + `  Use that broker:   set MQTT_URL=mqtt://localhost:${port}\n`
          + '  or pick another port for the built-in broker:   set MQTT_PORT=1884'));
      } else reject(err);
    });
    tcp.listen(port, () => resolve({
      port: tcp.address().port,
      close: () => new Promise((done) => { aedes.close(() => tcp.close(() => done())); }),
    }));
  });
}

async function start(config, { simOptions = {}, log = console.log } = {}) {
  const db = openDb(config.dataDir, { seedDemoData: config.seedDemoData });

  let broker = null;
  let mqttUrl = config.mqttUrl;
  if (!mqttUrl) {
    broker = await startEmbeddedBroker(config.mqttPort);
    mqttUrl = `mqtt://127.0.0.1:${broker.port}`;
    log(`MQTT broker (built-in) listening on port ${broker.port}`);
  } else {
    log(`Using external MQTT broker ${mqttUrl}`);
  }

  const parking = new ParkingService(db);
  parking.on('log', (line) => log(line));
  const connected = parking.connect(mqttUrl);
  if (broker) await connected; // an external broker may come up later; do not block on it

  let sim = null;
  if (config.simulator) {
    sim = new VirtualDevice({ url: mqttUrl, dataDir: config.dataDir, ...simOptions });
    sim.start();
    log('Hardware simulator is ON (turn it off in the dashboard before connecting the real ESP32)');
  }

  const server = createServer({ config, db, parking, sim });
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(config.httpPort, resolve);
  });
  const httpPort = server.address().port;
  log(`Dashboard: http://localhost:${httpPort}`);

  return {
    db, parking, sim, httpPort, mqttUrl,
    async close() {
      server.closeAllConnections();
      await new Promise((r) => { server.close(r); });
      if (sim) await sim.stop();
      await parking.close();
      if (broker) await broker.close();
      db.close();
    },
  };
}

module.exports = { start };
