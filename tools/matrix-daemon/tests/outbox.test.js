const { parseOutgoing } = require('../outbox');

describe('outbox.parseOutgoing', () => {
  test('accepts a well-formed JSON message with room+text', () => {
    const msg = parseOutgoing(Buffer.from(JSON.stringify({
      room: '!fleet:example.com',
      text: 'hello',
    })));
    expect(msg.room).toBe('!fleet:example.com');
    expect(msg.text).toBe('hello');
  });

  test('passes through optional fields like mention[] and thread_root', () => {
    const msg = parseOutgoing(Buffer.from(JSON.stringify({
      room: '!fleet:example.com',
      text: 'ping @snake',
      mention: ['@snake:example.com'],
      thread_root: '$root:example.com',
    })));
    expect(msg.mention).toEqual(['@snake:example.com']);
    expect(msg.thread_root).toBe('$root:example.com');
  });

  test('rejects empty buffer', () => {
    expect(() => parseOutgoing(Buffer.from(''))).toThrow(/empty/);
  });

  test('rejects malformed JSON', () => {
    expect(() => parseOutgoing(Buffer.from('{not json'))).toThrow(/bad JSON/);
  });

  test('rejects message missing room', () => {
    expect(() => parseOutgoing(Buffer.from(JSON.stringify({ text: 'hi' })))).toThrow(/room/);
  });

  test('rejects message missing text', () => {
    expect(() => parseOutgoing(Buffer.from(JSON.stringify({ room: '!x:y' })))).toThrow(/text/);
  });
});
