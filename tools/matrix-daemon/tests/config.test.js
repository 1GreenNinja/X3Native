const config = require('../config');

describe('config', () => {
  test('homeserver URL is HTTPS', () => {
    expect(config.homeserverUrl).toMatch(/^https:\/\//);
  });

  test('access token path is a non-empty string', () => {
    expect(typeof config.accessTokenPath).toBe('string');
    expect(config.accessTokenPath.length).toBeGreaterThan(0);
  });

  test('inbox path includes .matrix_inbox.jsonl', () => {
    expect(config.inboxPath).toContain('.matrix_inbox.jsonl');
  });

  test('pipe name starts with \\\\.\\pipe\\matrix-', () => {
    expect(config.pipeName).toMatch(/^\\\\\.\\pipe\\matrix-/);
  });

  test('machineName is lowercase non-empty', () => {
    expect(config.machineName).toMatch(/^[a-z0-9-]+/);
  });

  test('presenceIntervalMs is at least 1 minute', () => {
    expect(config.presenceIntervalMs).toBeGreaterThanOrEqual(60_000);
  });
});
