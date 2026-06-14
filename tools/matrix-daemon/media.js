// media.js — pure helpers for outbound image attachments.
//
// Kept dependency-free (no Pillow/sharp equivalent) so the daemon's only
// runtime deps stay matrix-bot-sdk + winston. Image dimensions are parsed
// straight from file headers; callers omit w/h when this returns 0×0.

const path = require('path');

const MIME_BY_EXT = {
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.jpeg': 'image/jpeg',
  '.gif': 'image/gif',
  '.webp': 'image/webp',
  '.bmp': 'image/bmp',
};

// Guess a MIME type from a filename's extension. Falls back to a generic
// binary type so an unknown extension still uploads (just without a nice
// preview hint).
function guessMimetype(filename) {
  return MIME_BY_EXT[path.extname(filename || '').toLowerCase()] || 'application/octet-stream';
}

// Parse pixel dimensions from a raw image buffer by sniffing the format
// header. Returns { width, height }; both 0 when the format is unknown or
// the header is too short to trust (Matrix clients still render without
// info.w/h — it only affects the placeholder aspect ratio).
function imageDimensions(buf) {
  const unknown = { width: 0, height: 0 };
  if (!Buffer.isBuffer(buf) || buf.length < 24) return unknown;

  // PNG: 8-byte signature, then IHDR with width/height as BE uint32 at 16/20.
  if (buf.length >= 24 &&
      buf[0] === 0x89 && buf[1] === 0x50 && buf[2] === 0x4e && buf[3] === 0x47) {
    return { width: buf.readUInt32BE(16), height: buf.readUInt32BE(20) };
  }

  // GIF: "GIF87a"/"GIF89a", logical-screen width/height as LE uint16 at 6/8.
  if (buf[0] === 0x47 && buf[1] === 0x49 && buf[2] === 0x46) {
    return { width: buf.readUInt16LE(6), height: buf.readUInt16LE(8) };
  }

  // BMP: "BM", width/height as LE int32 at 18/22 (height may be negative
  // for top-down bitmaps — report magnitude).
  if (buf[0] === 0x42 && buf[1] === 0x4d) {
    return { width: Math.abs(buf.readInt32LE(18)), height: Math.abs(buf.readInt32LE(22)) };
  }

  // JPEG: walk segments from offset 2 to the first Start-Of-Frame marker.
  if (buf[0] === 0xff && buf[1] === 0xd8) {
    let off = 2;
    while (off + 9 < buf.length) {
      if (buf[off] !== 0xff) { off += 1; continue; }
      let marker = buf[off + 1];
      // Skip fill bytes (0xff) and standalone markers without a length.
      while (marker === 0xff && off + 1 < buf.length) { off += 1; marker = buf[off + 1]; }
      const SOF = new Set([0xc0, 0xc1, 0xc2, 0xc3, 0xc5, 0xc6, 0xc7, 0xc9, 0xca, 0xcb, 0xcd, 0xce, 0xcf]);
      if (SOF.has(marker)) {
        // Segment: FF marker LEN(2) precision(1) height(2) width(2)
        return { width: buf.readUInt16BE(off + 7), height: buf.readUInt16BE(off + 5) };
      }
      // Otherwise advance past this segment using its length field.
      const segLen = buf.readUInt16BE(off + 2);
      if (segLen < 2) break;
      off += 2 + segLen;
    }
    return unknown;
  }

  // WEBP: "RIFF"<size>"WEBP"<fourcc>. Handle the three common chunk types.
  if (buf.length >= 30 &&
      buf.toString('ascii', 0, 4) === 'RIFF' && buf.toString('ascii', 8, 12) === 'WEBP') {
    const fourcc = buf.toString('ascii', 12, 16);
    if (fourcc === 'VP8X') {
      // 24-bit LE (width-1) at 24, (height-1) at 27.
      const w = 1 + (buf[24] | (buf[25] << 8) | (buf[26] << 16));
      const h = 1 + (buf[27] | (buf[28] << 8) | (buf[29] << 16));
      return { width: w, height: h };
    }
    if (fourcc === 'VP8 ') {
      // Lossy: 14-bit dimensions at offset 26/28 (after the 3-byte start code).
      const w = buf.readUInt16LE(26) & 0x3fff;
      const h = buf.readUInt16LE(28) & 0x3fff;
      if (w && h) return { width: w, height: h };
    }
    if (fourcc === 'VP8L' && buf[20] === 0x2f) {
      // Lossless: 14-bit (width-1) then (height-1) packed from offset 21.
      const b = buf.readUInt32LE(21);
      return { width: 1 + (b & 0x3fff), height: 1 + ((b >> 14) & 0x3fff) };
    }
  }

  return unknown;
}

module.exports = { guessMimetype, imageDimensions, MIME_BY_EXT };
