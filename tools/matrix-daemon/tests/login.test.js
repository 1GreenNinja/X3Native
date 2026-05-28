const fs = require('fs');
const path = require('path');
const os = require('os');
const { loadToken } = require('../login');

describe('login.loadToken', () => {
  const tmpTokenPath = path.join(os.tmpdir(), `matrix-daemon-test-token-${Date.now()}.txt`);

  afterEach(() => {
    if (fs.existsSync(tmpTokenPath)) fs.unlinkSync(tmpTokenPath);
  });

  test('strips trailing whitespace + newlines', () => {
    fs.writeFileSync(tmpTokenPath, 'syt_fake_test_token\n  \n');
    expect(loadToken(tmpTokenPath)).toBe('syt_fake_test_token');
  });

  test('throws when file missing', () => {
    expect(() => loadToken('/this/path/does/not/exist/token.txt')).toThrow(/not found/);
  });

  test('throws when file empty', () => {
    fs.writeFileSync(tmpTokenPath, '   \n');
    expect(() => loadToken(tmpTokenPath)).toThrow(/empty/);
  });
});

// createClient() is intentionally NOT unit-tested here — it requires the
// matrix-bot-sdk to construct a MatrixClient which spins up storage I/O.
// The daemon-level smoketest exercises it end-to-end.
