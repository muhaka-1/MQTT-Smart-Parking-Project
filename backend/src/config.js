'use strict';
// All settings come from environment variables so the same code runs on a
// laptop (npm start), on the Raspberry Pi, and inside Docker.
const path = require('node:path');

function fromEnv(env = process.env) {
  return {
    // Dashboard + REST API
    httpPort: Number(env.PORT) || 8080,
    // If MQTT_URL is empty the backend starts its own MQTT broker on mqttPort,
    // so no separate Mosquitto install is needed for local testing.
    mqttUrl: env.MQTT_URL || '',
    mqttPort: Number(env.MQTT_PORT) || 1883,
    // SQLite database file and the simulator's "flash memory" live here.
    dataDir: env.DATA_DIR || path.join(__dirname, '..', 'data'),
    adminPassword: env.ADMIN_PASSWORD || 'admin',
    // Virtual ESP32 for testing without hardware: on | off
    simulator: (env.SIMULATOR || 'on').toLowerCase() !== 'off',
    // Insert a few demo cards the first time the database is created.
    seedDemoData: (env.SEED_DEMO || 'on').toLowerCase() !== 'off',
  };
}

module.exports = { fromEnv };
