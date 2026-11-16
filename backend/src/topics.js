'use strict';
// MQTT contract shared by the backend, the simulator and the ESP32 firmware
// (src/main.cpp uses the same strings). See docs/MQTT_TOPICS.md.
module.exports = {
  // device -> backend
  ONLINE: 'parking/device/online',            // retained "online"/"offline" (offline = last will)
  STATUS: 'parking/status',                   // retained JSON snapshot
  ENTRY_LOG: 'parking/entry_log',             // JSON event
  EXIT_LOG: 'parking/exit_log',               // JSON event
  DENIED_LOG: 'parking/denied_log',           // JSON event
  CACHE_POLL: 'parking/cache/poll_request',   // payload = whitelist version the device holds
  OCCUPIED_REQUEST: 'parking/occupied_persistent/request',

  // backend -> device
  ACK: 'parking/ack',                         // payload = event id that is now safely stored
  TIME: 'parking/time',                       // payload = unix time in seconds
  CARDS_VERSION: 'parking/cards_version',     // retained whitelist version number
  CACHE_UPDATE: 'parking/cache/update',       // retained "v=<n>\nUID|Owner\n..."
  OCCUPIED_RESPONSE: 'parking/occupied_persistent/response',
  CMD: 'parking/cmd',                         // JSON {cmd, value}
};
