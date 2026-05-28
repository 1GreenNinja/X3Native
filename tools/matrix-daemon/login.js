// login.js — token loading + MatrixClient construction. Kept as a thin
// wrapper around matrix-bot-sdk so it can be unit-tested independently
// of the network sync loop.

const fs = require('fs');
const path = require('path');
const os = require('os');

// Defer requiring matrix-bot-sdk until createClient() is called, so the
// pure helpers (loadToken) can be tested without the heavy SDK import.
function _sdk() {
  return require('matrix-bot-sdk');
}

function loadToken(tokenPath) {
  if (!fs.existsSync(tokenPath)) {
    throw new Error(`matrix access token file not found at ${tokenPath}`);
  }
  const raw = fs.readFileSync(tokenPath, 'utf8').trim();
  if (!raw) {
    throw new Error(`matrix access token file is empty at ${tokenPath}`);
  }
  return raw;
}

function createClient(homeserverUrl, accessToken, storagePath) {
  const sdk = _sdk();
  const { MatrixClient, SimpleFsStorageProvider, AutojoinRoomsMixin } = sdk;
  const resolvedStorage = storagePath || path.join(os.homedir(), '.claude', 'matrix-daemon-storage.json');
  const storage = new SimpleFsStorageProvider(resolvedStorage);
  const client = new MatrixClient(homeserverUrl, accessToken, storage);
  AutojoinRoomsMixin.setupOnClient(client);
  return client;
}

module.exports = { loadToken, createClient };
