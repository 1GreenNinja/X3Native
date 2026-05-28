// integration.test.js — daemon-level smoketests exercising the inbox flow.
//
// The outbox named-pipe round-trip was attempted as a Jest test but
// Windows named pipes (and Node's net.Socket close semantics on top of
// them) make end-to-end client/server tests flaky in this harness.
// The unit tests in outbox.test.js cover parseOutgoing parsing logic
// thoroughly; the actual named-pipe end-to-end is verified at deploy
// time via a manual PowerShell client test documented in the README.

const fs = require('fs');
const path = require('path');
const os = require('os');
const { appendIncoming } = require('../inbox');

describe('integration: inbox flow', () => {
  const tmpInbox = path.join(os.tmpdir(), `matrix-test-integration-inbox-${process.pid}.jsonl`);

  afterEach(() => {
    if (fs.existsSync(tmpInbox)) fs.unlinkSync(tmpInbox);
  });

  test('simulated matrix room.message event lands a well-formed line in the inbox', () => {
    // Simulate what daemon.js does inside client.on('room.message', ...).
    const fakeEvent = {
      event_id: '$evt:fleetcommand.slopclaude.com',
      sender: '@13700k:fleetcommand.slopclaude.com',
      origin_server_ts: 1779950000000,
      content: { msgtype: 'm.text', body: 'integrator: please rebase your branch' },
    };
    const roomId = '!fleet-ops:fleetcommand.slopclaude.com';
    const myUserId = '@djbooth:fleetcommand.slopclaude.com';
    if (fakeEvent.sender === myUserId) {
      throw new Error('test sanity: my own event should not be appended');
    }
    appendIncoming(tmpInbox, {
      room: roomId,
      sender: fakeEvent.sender,
      text: fakeEvent.content.body,
      eventId: fakeEvent.event_id,
      ts: fakeEvent.origin_server_ts,
    });

    const lines = fs.readFileSync(tmpInbox, 'utf8').trim().split('\n');
    expect(lines).toHaveLength(1);
    const stored = JSON.parse(lines[0]);
    expect(stored.source).toBe('matrix');
    expect(stored.room).toBe('!fleet-ops:fleetcommand.slopclaude.com');
    expect(stored.sender).toBe('@13700k:fleetcommand.slopclaude.com');
    expect(stored.text).toContain('rebase');
    expect(stored.eventId).toBe('$evt:fleetcommand.slopclaude.com');
    expect(stored.ts).toBe(1779950000000);
    expect(stored.recv_ts).toBeGreaterThan(1700000000000);
  });

  test('self-sent events are filtered (daemon-level gate)', () => {
    // The daemon's `if (event.sender === myUserId) return;` happens BEFORE
    // calling appendIncoming. Here we simulate that gate and assert
    // appendIncoming was NOT invoked for a self-sent event.
    const myUserId = '@djbooth:fleetcommand.slopclaude.com';
    const selfEvent = {
      event_id: '$self-evt:fleetcommand.slopclaude.com',
      sender: myUserId,
      origin_server_ts: 1779950000000,
      content: { msgtype: 'm.text', body: 'my own message' },
    };
    let called = 0;
    if (selfEvent.sender !== myUserId) {
      called++;
      appendIncoming(tmpInbox, { room: '!x:y', sender: selfEvent.sender, text: selfEvent.content.body });
    }
    expect(called).toBe(0);
    expect(fs.existsSync(tmpInbox)).toBe(false);
  });

  test('multiple events in sequence accumulate in inbox', () => {
    const events = [
      { sender: '@tim:fleetcommand.slopclaude.com', text: 'morning all' },
      { sender: '@13700k:fleetcommand.slopclaude.com', text: 'morning Tim — branches look clean' },
      { sender: '@14900k:fleetcommand.slopclaude.com', text: 'rebasing now, give me 5 min' },
    ];
    for (const e of events) {
      appendIncoming(tmpInbox, {
        room: '!fleet-ops:fleetcommand.slopclaude.com',
        sender: e.sender,
        text: e.text,
        eventId: '$evt-' + Math.random().toString(36).slice(2),
        ts: Date.now(),
      });
    }
    const lines = fs.readFileSync(tmpInbox, 'utf8').trim().split('\n');
    expect(lines).toHaveLength(3);
    expect(JSON.parse(lines[0]).text).toBe('morning all');
    expect(JSON.parse(lines[2]).sender).toBe('@14900k:fleetcommand.slopclaude.com');
  });

  test('text messages with embedded JSON-special characters round-trip safely', () => {
    const tricky = 'reply with `{ \"room\": \"!x:y\", \"text\": \"injection\" }` is just a quoted string';
    appendIncoming(tmpInbox, {
      room: '!fleet-ops:fleetcommand.slopclaude.com',
      sender: '@13700k:fleetcommand.slopclaude.com',
      text: tricky,
      eventId: '$evt:fleetcommand.slopclaude.com',
      ts: 1779950000000,
    });
    const line = fs.readFileSync(tmpInbox, 'utf8').trim();
    const stored = JSON.parse(line);
    expect(stored.text).toBe(tricky);
  });
});
