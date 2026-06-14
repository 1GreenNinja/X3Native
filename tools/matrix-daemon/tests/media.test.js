const { guessMimetype, imageDimensions } = require('../media');

describe('media.guessMimetype', () => {
  test('maps known image extensions', () => {
    expect(guessMimetype('shot.png')).toBe('image/png');
    expect(guessMimetype('a.jpg')).toBe('image/jpeg');
    expect(guessMimetype('a.jpeg')).toBe('image/jpeg');
    expect(guessMimetype('loop.gif')).toBe('image/gif');
    expect(guessMimetype('pic.webp')).toBe('image/webp');
  });

  test('is case-insensitive on the extension', () => {
    expect(guessMimetype('RENDER.PNG')).toBe('image/png');
    expect(guessMimetype('Photo.JPG')).toBe('image/jpeg');
  });

  test('falls back to octet-stream for unknown/missing extensions', () => {
    expect(guessMimetype('data.bin')).toBe('application/octet-stream');
    expect(guessMimetype('noext')).toBe('application/octet-stream');
    expect(guessMimetype('')).toBe('application/octet-stream');
  });
});

describe('media.imageDimensions', () => {
  test('parses a PNG header (BE uint32 at 16/20)', () => {
    const buf = Buffer.alloc(24);
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]).copy(buf, 0);
    buf.write('IHDR', 12, 'ascii');
    buf.writeUInt32BE(1600, 16);
    buf.writeUInt32BE(1350, 20);
    expect(imageDimensions(buf)).toEqual({ width: 1600, height: 1350 });
  });

  test('parses a GIF header (LE uint16 at 6/8)', () => {
    const buf = Buffer.alloc(24);
    buf.write('GIF89a', 0, 'ascii');
    buf.writeUInt16LE(640, 6);
    buf.writeUInt16LE(480, 8);
    expect(imageDimensions(buf)).toEqual({ width: 640, height: 480 });
  });

  test('parses a BMP header (LE int32 at 18/22, magnitude)', () => {
    const buf = Buffer.alloc(26);
    buf.write('BM', 0, 'ascii');
    buf.writeInt32LE(800, 18);
    buf.writeInt32LE(-600, 22); // top-down bitmaps store negative height
    expect(imageDimensions(buf)).toEqual({ width: 800, height: 600 });
  });

  test('parses a JPEG SOF0 segment (height then width, BE)', () => {
    // FFD8  FFC0 0011 08  HHHH WWWW  ...component bytes (padding)
    const buf = Buffer.alloc(32);
    buf[0] = 0xff; buf[1] = 0xd8;
    buf[2] = 0xff; buf[3] = 0xc0;
    buf.writeUInt16BE(0x0011, 4); // segment length
    buf[6] = 0x08;                // sample precision
    buf.writeUInt16BE(720, 7);    // height
    buf.writeUInt16BE(1280, 9);   // width
    expect(imageDimensions(buf)).toEqual({ width: 1280, height: 720 });
  });

  test('returns 0×0 for unknown/short buffers', () => {
    expect(imageDimensions(Buffer.from('not an image at all....'))).toEqual({ width: 0, height: 0 });
    expect(imageDimensions(Buffer.alloc(4))).toEqual({ width: 0, height: 0 });
    expect(imageDimensions('notabuffer')).toEqual({ width: 0, height: 0 });
  });
});
