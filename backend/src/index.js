'use strict';
const [major, minor] = process.versions.node.split('.').map(Number);
if (major < 22 || (major === 22 && minor < 13)) {
  console.error(`Node.js 22.13 or newer is required (you have ${process.versions.node}). Install the current LTS from https://nodejs.org or use Docker.`);
  process.exit(1);
}

const { fromEnv } = require('./config');
const { start } = require('./app');

const config = fromEnv();
start(config).then((app) => {
  if (config.adminPassword === 'admin') {
    console.log('Sign in with password "admin". Set ADMIN_PASSWORD to change it.');
  }
  const stop = () => app.close().finally(() => process.exit(0));
  process.on('SIGINT', stop);
  process.on('SIGTERM', stop);
}).catch((err) => {
  console.error(`Could not start: ${err.message}`);
  process.exit(1);
});
