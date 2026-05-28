// inbox.js — append-only writer for the local Matrix inbox file.
// Mirrors the shape the Slack daemon writes to .slack_inbox.jsonl so the
// /loop Claude-session handler can drain a unified inbox in the future.

const fs = require('fs');

function appendIncoming(inboxPath, message) {
  // Defensive validation — never corrupt the inbox with non-JSON-serializable
  // input, and always include an arrival timestamp the consumer can sort on.
  const enriched = {
    source: 'matrix',
    ...message,
    recv_ts: message.recv_ts || Date.now(),
  };
  const line = JSON.stringify(enriched) + '\n';
  fs.appendFileSync(inboxPath, line, { encoding: 'utf8' });
  return enriched;
}

module.exports = { appendIncoming };
