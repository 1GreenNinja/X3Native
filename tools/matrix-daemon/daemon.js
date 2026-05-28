// daemon.js — the per-machine matrix-bot-sdk daemon entry point.
//
// Boot sequence:
//   1. Load config (env-driven, defaults from config.js)
//   2. Load access token from disk (~/.claude/.matrix_token)
//   3. Create MatrixClient + register inbox writer for incoming events
//   4. Start outbox named-pipe listener so the local Claude session can post
//   5. Schedule periodic presence emission so fleet machines see this PC alive
//   6. Begin sync (blocks; matrix-bot-sdk reconnects on its own)
//
// On any fatal init error, log + exit nonzero. The Scheduled Task wrapper
// auto-restarts after a 1-minute backoff (RestartCount=5).

const config = require('./config');
const { loadToken, createClient } = require('./login');
const { appendIncoming } = require('./inbox');
const { startOutboxListener } = require('./outbox');
const winston = require('winston');

const logger = winston.createLogger({
  format: winston.format.combine(winston.format.timestamp(), winston.format.json()),
  transports: [
    new winston.transports.Console(),
    new winston.transports.File({ filename: config.logPath, maxsize: 5_000_000, maxFiles: 3 }),
  ],
});

async function main() {
  logger.info({ event: 'daemon-init', machine: config.machineName, homeserver: config.homeserverUrl });

  const token = loadToken(config.accessTokenPath);
  const client = createClient(config.homeserverUrl, token, config.storagePath);

  const myUserId = await client.getUserId();
  logger.info({ event: 'identified', user_id: myUserId });

  // ---- inbox writer on every text message in joined rooms or DMs ----
  client.on('room.message', async (roomId, event) => {
    try {
      if (!event.content || event.content.msgtype !== 'm.text') return;
      if (event.sender === myUserId) return; // skip our own posts
      appendIncoming(config.inboxPath, {
        room: roomId,
        sender: event.sender,
        text: event.content.body || '',
        eventId: event.event_id,
        ts: event.origin_server_ts,
      });
      logger.info({ event: 'inbox-append', room: roomId, sender: event.sender });
    } catch (e) {
      logger.warn({ event: 'inbox-write-fail', error: e.message });
    }
  });

  // ---- outbox: named-pipe server accepts JSON from the local Claude session ----
  startOutboxListener(config.pipeName, async (msg) => {
    const content = {
      msgtype: 'm.text',
      body: msg.text,
    };
    if (msg.mention && msg.mention.length) {
      content['m.mentions'] = { user_ids: msg.mention };
    }
    let payload = content;
    if (msg.thread_root) {
      payload = {
        ...content,
        'm.relates_to': { 'rel_type': 'm.thread', 'event_id': msg.thread_root },
      };
    }
    const eventId = await client.sendMessage(msg.room, payload);
    logger.info({ event: 'outbox-sent', room: msg.room, eventId });
    return { eventId };
  });
  logger.info({ event: 'outbox-listening', pipe: config.pipeName });

  // ---- presence: emit "online" every 5 min so peers see us alive ----
  const presenceTick = async () => {
    try {
      await client.setPresenceStatus('online', `${config.machineName} alive @ ${new Date().toISOString()}`);
    } catch (e) {
      logger.warn({ event: 'presence-fail', error: e.message });
    }
  };
  setInterval(presenceTick, config.presenceIntervalMs);
  presenceTick();

  logger.info({ event: 'starting-sync' });
  await client.start();
  logger.info({ event: 'sync-started' });
}

main().catch((err) => {
  logger.error({ event: 'fatal', error: err.message, stack: err.stack });
  process.exit(1);
});

// Graceful shutdown logging — Scheduled Task will respawn on exit.
process.on('SIGTERM', () => { logger.info({ event: 'SIGTERM' }); process.exit(0); });
process.on('SIGINT', () => { logger.info({ event: 'SIGINT' }); process.exit(0); });
