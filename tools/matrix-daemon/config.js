// config.js — runtime configuration for the per-machine matrix-bot-sdk daemon.
// Reads env vars with sensible defaults. The CLAUDE_DIR + machine name drive
// all derived paths so the same code runs identically on every fleet PC.

const os = require('os');
const path = require('path');

const machineName = (process.env.MATRIX_BOT_MACHINE || os.hostname()).toLowerCase();
const claudeDir = process.env.CLAUDE_DIR || path.join(os.homedir(), '.claude');

module.exports = {
  homeserverUrl: process.env.MATRIX_HOMESERVER_URL || 'https://chat.tims-fleet.xyz',
  accessTokenPath: path.join(claudeDir, '.matrix_token'),
  inboxPath: path.join(claudeDir, '.matrix_inbox.jsonl'),
  outboxLogPath: path.join(claudeDir, '.matrix_outbox.jsonl'),
  seenPath: path.join(claudeDir, '.matrix_seen.json'),
  logPath: path.join(claudeDir, '.matrix-daemon.log'),
  storagePath: path.join(claudeDir, 'matrix-daemon-storage.json'),
  pipeName: `\\\\.\\pipe\\matrix-${machineName}`,
  machineName,
  claudeDir,
  presenceIntervalMs: 5 * 60 * 1000,
  reconnectBaseMs: 1000,
  reconnectMaxMs: 60_000,
};
