const fs = require('fs');
const path = require('path');
const os = require('os');
const { appendIncoming } = require('../inbox');

describe('inbox.appendIncoming', () => {
  const tmpInbox = path.join(os.tmpdir(), `matrix-daemon-test-inbox-${Date.now()}.jsonl`);

  afterEach(() => {
    if (fs.existsSync(tmpInbox)) fs.unlinkSync(tmpInbox);
  });

  test('writes one JSON line per message', () => {
    appendIncoming(tmpInbox, {
      room: '!fleet:example.com',
      sender: '@tim:example.com',
      text: 'hello djbooth',
      eventId: '$evt1',
      ts: 1700000000000,
    });
    appendIncoming(tmpInbox, {
      room: '!fleet:example.com',
      sender: '@13700k:example.com',
      text: 'how is gating?',
      eventId: '$evt2',
      ts: 1700000001000,
    });
    const lines = fs.readFileSync(tmpInbox, 'utf8').trim().split('\n');
    expect(lines).toHaveLength(2);
    expect(JSON.parse(lines[0]).text).toBe('hello djbooth');
    expect(JSON.parse(lines[1]).sender).toBe('@13700k:example.com');
  });

  test('auto-stamps source=matrix and recv_ts if missing', () => {
    const stored = appendIncoming(tmpInbox, {
      room: '!x:y.com',
      sender: '@a:y.com',
      text: 'hi',
    });
    expect(stored.source).toBe('matrix');
    expect(stored.recv_ts).toBeGreaterThan(1700000000000);
  });

  test('preserves explicit recv_ts if caller provided one', () => {
    const ts = 1699999999000;
    const stored = appendIncoming(tmpInbox, {
      room: '!x:y.com',
      sender: '@a:y.com',
      text: 'hi',
      recv_ts: ts,
    });
    expect(stored.recv_ts).toBe(ts);
  });
});
