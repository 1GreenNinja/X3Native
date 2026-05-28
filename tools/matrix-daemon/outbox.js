// outbox.js — named-pipe (Windows) server that accepts outbound message
// requests from the local Claude Code session and dispatches them via the
// Matrix client. Each connection sends one JSON object, gets one JSON reply.

const net = require('net');

function parseOutgoing(buffer) {
  const text = buffer.toString('utf8').trim();
  if (!text) throw new Error('outbox message empty');
  let msg;
  try {
    msg = JSON.parse(text);
  } catch (e) {
    throw new Error(`outbox bad JSON: ${e.message}`);
  }
  if (!msg.room) throw new Error('outbox message missing field: room');
  if (!msg.text) throw new Error('outbox message missing field: text');
  return msg;
}

function startOutboxListener(pipeName, onMessage) {
  const server = net.createServer((socket) => {
    const chunks = [];
    socket.on('data', (d) => chunks.push(d));
    socket.on('end', () => {
      const buf = Buffer.concat(chunks);
      let msg;
      try {
        msg = parseOutgoing(buf);
      } catch (e) {
        try { socket.write(JSON.stringify({ ok: false, error: e.message })); } catch (_) {}
        try { socket.end(); } catch (_) {}
        return;
      }
      Promise.resolve()
        .then(() => onMessage(msg))
        .then(
          (result) => socket.write(JSON.stringify({ ok: true, ...result })),
          (err) => socket.write(JSON.stringify({ ok: false, error: err.message }))
        )
        .finally(() => {
          try { socket.end(); } catch (_) {}
        });
    });
    socket.on('error', (_e) => { /* swallow per-connection errors */ });
  });
  server.listen(pipeName);
  return server;
}

module.exports = { parseOutgoing, startOutboxListener };
