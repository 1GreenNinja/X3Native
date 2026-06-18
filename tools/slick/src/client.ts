// client.ts — Slick's thin Matrix client. Hand-rolled on purpose:
// no E2EE / no federation on the fleet homeserver, so matrix-js-sdk's
// weight buys nothing here (see spec §4.1). Everything is plain fetch
// against Conduit's client-server REST API.

export const HOMESERVER = "https://fleetcommand.slopclaude.com";

export interface MatrixEvent {
  event_id: string;
  type: string;
  sender: string;
  origin_server_ts: number;
  content: Record<string, any>;
  state_key?: string;
  unsigned?: { transaction_id?: string };
}

export interface SyncRoomData {
  timeline: { events: MatrixEvent[]; prev_batch?: string };
  state: { events: MatrixEvent[] };
  unread_notifications?: { notification_count?: number };
}

export interface SyncResponse {
  next_batch: string;
  rooms?: { join?: Record<string, SyncRoomData> };
}

export class MatrixError extends Error {
  constructor(
    public status: number,
    public errcode: string,
    message: string,
  ) {
    super(message);
  }
}

async function req<T>(
  path: string,
  opts: { method?: string; token?: string; body?: unknown; signal?: AbortSignal } = {},
): Promise<T> {
  const headers: Record<string, string> = {};
  if (opts.token) headers["Authorization"] = `Bearer ${opts.token}`;
  if (opts.body !== undefined) headers["Content-Type"] = "application/json";
  const resp = await fetch(`${HOMESERVER}${path}`, {
    method: opts.method ?? "GET",
    headers,
    body: opts.body !== undefined ? JSON.stringify(opts.body) : undefined,
    signal: opts.signal,
  });
  const data = await resp.json().catch(() => ({}));
  if (!resp.ok) {
    throw new MatrixError(
      resp.status,
      data.errcode ?? "M_UNKNOWN",
      data.error ?? `HTTP ${resp.status}`,
    );
  }
  return data as T;
}

export interface Session {
  userId: string;
  token: string;
  deviceId: string;
}

const SESSION_KEY = "slick.session";
const SINCE_KEY = "slick.since";

// Module-level token cache so mxcToUrl() can append ?access_token= for <img>
// tags (which can't send Authorization headers). Set on every login path.
let _authToken = "";
function rememberToken(t: string): void { _authToken = t; }

export function loadSession(): Session | null {
  try {
    const raw = localStorage.getItem(SESSION_KEY);
    if (!raw) return null;
    const s = JSON.parse(raw) as Session;
    if (s?.token) rememberToken(s.token);
    return s;
  } catch {
    return null;
  }
}

export function clearSession(): void {
  localStorage.removeItem(SESSION_KEY);
  localStorage.removeItem(SINCE_KEY);
}

export async function login(username: string, password: string): Promise<Session> {
  const data = await req<{ user_id: string; access_token: string; device_id: string }>(
    "/_matrix/client/v3/login",
    {
      method: "POST",
      body: {
        type: "m.login.password",
        identifier: { type: "m.id.user", user: username },
        password,
        initial_device_display_name: "Slick",
      },
    },
  );
  const session: Session = {
    userId: data.user_id,
    token: data.access_token,
    deviceId: data.device_id,
  };
  rememberToken(session.token);
  localStorage.setItem(SESSION_KEY, JSON.stringify(session));
  return session;
}

/** Sign in with an existing access token (no password needed). Validates the
 * token via /account/whoami, then persists the session. The escape hatch for
 * "I don't know my password" — paste the token from Element. */
export async function loginWithToken(token: string): Promise<Session> {
  const who = await req<{ user_id: string; device_id?: string }>(
    "/_matrix/client/v3/account/whoami",
    { token },
  );
  const session: Session = {
    userId: who.user_id,
    token: token.trim(),
    deviceId: who.device_id ?? "slick-token",
  };
  rememberToken(session.token);
  localStorage.setItem(SESSION_KEY, JSON.stringify(session));
  return session;
}

/** One /sync long-poll. First call (no since) gets a full snapshot. */
export async function sync(
  token: string,
  since: string | null,
  signal?: AbortSignal,
): Promise<SyncResponse> {
  const params = new URLSearchParams({ timeout: since ? "30000" : "0" });
  if (since) params.set("since", since);
  return req<SyncResponse>(`/_matrix/client/v3/sync?${params}`, { token, signal });
}

export function loadSince(): string | null {
  return localStorage.getItem(SINCE_KEY);
}

export function saveSince(since: string): void {
  localStorage.setItem(SINCE_KEY, since);
}

let txnCounter = 0;

export function newTxnId(): string {
  return `slick-${Date.now()}-${txnCounter++}`;
}

export async function sendMessage(
  token: string,
  roomId: string,
  content: Record<string, any>,
  txn: string,
  type = "m.room.message",
): Promise<string> {
  const data = await req<{ event_id: string }>(
    `/_matrix/client/v3/rooms/${encodeURIComponent(roomId)}/send/${type}/${txn}`,
    { method: "PUT", token, body: content },
  );
  return data.event_id;
}

/** Resolve a room alias (#admins:server) to its room id. */
export async function resolveAlias(token: string, alias: string): Promise<string> {
  const data = await req<{ room_id: string }>(
    `/_matrix/client/v3/directory/room/${encodeURIComponent(alias)}`,
    { token },
  );
  return data.room_id;
}

/** Run a Conduit admin command via the #admins room. The bot only reads
 * messages that MENTION it, so we ping @conduit in both the body and
 * m.mentions, then poll for its reply. Only works if the logged-in user is the
 * server admin (bot ignores others). Returns the bot's reply text. */
export async function adminCommand(
  token: string,
  userId: string,
  command: string,
): Promise<string> {
  const server = userId.split(":")[1];
  const conduit = `@conduit:${server}`;
  const roomId = await resolveAlias(token, `#admins:${server}`);
  await sendMessage(token, roomId, {
    msgtype: "m.text",
    body: `${conduit}: ${command}`,
    "m.mentions": { user_ids: [conduit] },
  }, newTxnId());

  // poll up to ~6s for the bot's reply
  for (let i = 0; i < 4; i++) {
    await new Promise((r) => setTimeout(r, 1500));
    try {
      const msgs = await req<{ chunk: MatrixEvent[] }>(
        `/_matrix/client/v3/rooms/${encodeURIComponent(roomId)}/messages?dir=b&limit=8`,
        { token },
      );
      const reply = (msgs.chunk ?? []).find(
        (e) => e.sender === conduit && e.type === "m.room.message",
      );
      if (reply) return reply.content?.body ?? "(done)";
    } catch { /* keep polling */ }
  }
  return "Command sent — no reply yet. Check the #admins room in a moment.";
}

/** Is the logged-in user the server admin? (member of #admins.) */
export async function isServerAdmin(token: string, userId: string): Promise<boolean> {
  const server = userId.split(":")[1];
  try {
    const roomId = await resolveAlias(token, `#admins:${server}`);
    const joined = await req<{ joined_rooms: string[] }>("/_matrix/client/v3/joined_rooms", { token });
    return (joined.joined_rooms ?? []).includes(roomId);
  } catch {
    return false;
  }
}

/** Change the logged-in user's password. One call, one job.
 *
 * Matrix needs the CURRENT password to re-auth (UIAA m.login.password): we
 * probe once to get the UIAA session id, then submit current + new together.
 * logout_devices:false keeps the fleet bots' sessions alive. Returns a fresh
 * token for THIS device so the live session keeps working after the change. */
export async function changePassword(
  token: string,
  userId: string,
  currentPassword: string,
  newPassword: string,
): Promise<void> {
  const path = `${HOMESERVER}/_matrix/client/v3/account/password`;
  const localpart = userId.split(":")[0].replace(/^@/, "");
  const post = (body: unknown) =>
    fetch(path, {
      method: "POST",
      headers: { "Content-Type": "application/json", Authorization: `Bearer ${token}` },
      body: JSON.stringify(body),
    });

  // Step 1 — probe to get the UIAA session (Conduit answers 401 + {session})
  const probe = await post({ new_password: newPassword, logout_devices: false });
  if (probe.ok) return; // server accepted with no challenge (rare)
  const pb = await probe.json().catch(() => ({}));
  const session: string | undefined = pb.session;
  if (!session) {
    throw new MatrixError(probe.status, pb.errcode ?? "M_UNKNOWN", pb.error ?? `HTTP ${probe.status}`);
  }

  // Step 2 — submit current-password auth + the new password
  const resp = await post({
    auth: { type: "m.login.password", identifier: { type: "m.id.user", user: localpart }, password: currentPassword, session },
    new_password: newPassword,
    logout_devices: false,
  });
  if (!resp.ok) {
    const eb = await resp.json().catch(() => ({}));
    // Friendlier message for the most common failure
    if (resp.status === 401 || eb.errcode === "M_FORBIDDEN") {
      throw new MatrixError(resp.status, "M_FORBIDDEN", "Current password is incorrect.");
    }
    throw new MatrixError(resp.status, eb.errcode ?? "M_UNKNOWN", eb.error ?? `HTTP ${resp.status}`);
  }
}

/** Resolve an mxc:// URI to a full-res download URL an <img> tag can load.
 * The token goes in the query string because <img> can't send a Bearer
 * header. Uses the legacy /media/v3/download endpoint (Conduit 0.10.12 serves
 * originals there; DJBOOTH killed the thumbnail substitution server-side). */
export function mxcToUrl(mxc: string): string {
  const m = /^mxc:\/\/([^/]+)\/(.+)$/.exec(mxc);
  if (!m) return "";
  const base = `${HOMESERVER}/_matrix/media/v3/download/${m[1]}/${m[2]}`;
  return _authToken ? `${base}?access_token=${encodeURIComponent(_authToken)}` : base;
}

/** Upload a file at ORIGINAL quality (no client recompression). Returns mxc://.
 * Auth via ?access_token= query param, NOT a Bearer header: Slick runs
 * cross-origin (slick.* -> fleetcommand.*) and the media endpoint drops the
 * Authorization header in that case (M_MISSING_TOKEN). The query param survives. */
export async function uploadMedia(token: string, file: File): Promise<string> {
  const url =
    `${HOMESERVER}/_matrix/media/v3/upload` +
    `?filename=${encodeURIComponent(file.name)}&access_token=${encodeURIComponent(token)}`;
  const resp = await fetch(url, {
    method: "POST",
    headers: { "Content-Type": file.type || "application/octet-stream" },
    body: file,
  });
  const data = await resp.json().catch(() => ({}));
  if (!resp.ok) {
    throw new MatrixError(resp.status, data.errcode ?? "M_UNKNOWN", data.error ?? `HTTP ${resp.status}`);
  }
  return data.content_uri as string;
}

export interface RoomMember {
  userId: string;
  displayName: string;
}

/** Joined members of a room, for the member panel. */
export async function joinedMembers(token: string, roomId: string): Promise<RoomMember[]> {
  const data = await req<{ joined: Record<string, { display_name?: string }> }>(
    `/_matrix/client/v3/rooms/${encodeURIComponent(roomId)}/joined_members`,
    { token },
  );
  return Object.entries(data.joined ?? {}).map(([userId, info]) => ({
    userId,
    displayName: info.display_name || userId.split(":")[0].replace(/^@/, ""),
  }));
}

/** Presence for a user: "online" | "offline" | "unavailable". */
export async function getPresence(
  token: string,
  userId: string,
): Promise<{ presence: string; lastActiveAgo?: number }> {
  try {
    const data = await req<{ presence: string; last_active_ago?: number }>(
      `/_matrix/client/v3/presence/${encodeURIComponent(userId)}/status`,
      { token },
    );
    return { presence: data.presence ?? "offline", lastActiveAgo: data.last_active_ago };
  } catch {
    return { presence: "offline" };
  }
}

/** Read an image File's pixel dimensions for the m.image info block. */
export function imageDimensions(file: File): Promise<{ w: number; h: number }> {
  return new Promise((resolve) => {
    const url = URL.createObjectURL(file);
    const img = new Image();
    img.onload = () => {
      resolve({ w: img.naturalWidth, h: img.naturalHeight });
      URL.revokeObjectURL(url);
    };
    img.onerror = () => {
      resolve({ w: 0, h: 0 });
      URL.revokeObjectURL(url);
    };
    img.src = url;
  });
}
