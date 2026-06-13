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

export function loadSession(): Session | null {
  try {
    const raw = localStorage.getItem(SESSION_KEY);
    return raw ? (JSON.parse(raw) as Session) : null;
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

/** Resolve an mxc:// URI to the authenticated full-res download URL. */
export function mxcToUrl(mxc: string): string {
  const m = /^mxc:\/\/([^/]+)\/(.+)$/.exec(mxc);
  if (!m) return "";
  return `${HOMESERVER}/_matrix/client/v1/media/download/${m[1]}/${m[2]}`;
}

/** Upload a file at ORIGINAL quality (no client recompression). Returns mxc://. */
export async function uploadMedia(token: string, file: File): Promise<string> {
  const resp = await fetch(
    `${HOMESERVER}/_matrix/media/v3/upload?filename=${encodeURIComponent(file.name)}`,
    {
      method: "POST",
      headers: {
        Authorization: `Bearer ${token}`,
        "Content-Type": file.type || "application/octet-stream",
      },
      body: file,
    },
  );
  const data = await resp.json().catch(() => ({}));
  if (!resp.ok) {
    throw new MatrixError(resp.status, data.errcode ?? "M_UNKNOWN", data.error ?? `HTTP ${resp.status}`);
  }
  return data.content_uri as string;
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
