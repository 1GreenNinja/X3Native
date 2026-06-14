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

  test('rejects message missing both text and image', () => {
    expect(() => parseOutgoing(Buffer.from(JSON.stringify({ room: '!x:y' })))).toThrow(/text/);
  });

  test('accepts an image-only message (no text required)', () => {
    const msg = parseOutgoing(Buffer.from(JSON.stringify({
      room: '!fleet:example.com',
      image: 'C:/shots/render.png',
    })));
    expect(msg.image).toBe('C:/shots/render.png');
    expect(msg.text).toBeUndefined();
  });

  test('accepts an image with a text caption', () => {
    const msg = parseOutgoing(Buffer.from(JSON.stringify({
      room: '!fleet:example.com',
      image: 'C:/shots/render.png',
      text: "Jake's ship, full-res",
    })));
    expect(msg.image).toBe('C:/shots/render.png');
    expect(msg.text).toBe("Jake's ship, full-res");
  });
});
