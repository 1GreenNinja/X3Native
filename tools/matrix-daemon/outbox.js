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
  // allowHalfOpen so the server can still write the reply after the client
  // half-closes (its end() shouldn't tear down the whole socket — we need
  // the response path to remain open until our write is flushed).
  const server = net.createServer({ allowHalfOpen: true }, (socket) => {
    const chunks = [];
    socket.on('data', (d) => chunks.push(d));
    socket.on('error', (_e) => { /* swallow per-connection errors */ });

    const replyAndClose = (obj) => {
      // Combined end(data) guarantees the OS flushes the payload before
      // closing the socket — avoids the race where socket.write() then
      // socket.end() drops the write on Windows named pipes.
      try { socket.end(JSON.stringify(obj)); } catch (_) {}
    };

    socket.on('end', () => {
      // Unified Promise chain: both error and success paths complete in
      // a microtask, giving identical timing semantics for the client's
      // listener wiring. Sync errors from parseOutgoing land in the catch.
      const buf = Buffer.concat(chunks);
      Promise.resolve()
        .then(() => {
          const msg = parseOutgoing(buf);  // throws -> handled by rejection branch
          return onMessage(msg);
        })
        .then(
          (result) => replyAndClose({ ok: true, ...result }),
          (err) => replyAndClose({ ok: false, error: err.message })
        );
    });
  });
  server.listen(pipeName);
  return server;
}

module.exports = { parseOutgoing, startOutboxListener };
