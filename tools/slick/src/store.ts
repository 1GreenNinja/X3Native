// store.ts — Slick's whole state model: a room map fed by one /sync loop.
// No state library; Preact components subscribe via a version counter.

import {
  type MatrixEvent,
  type Session,
  type SyncResponse,
  sync,
  saveSince,
  joinedMembers,
} from "./client";

export interface Room {
  id: string;
  name: string;
  isDm: boolean;
  needsName?: boolean;  // DM whose name still needs an API member lookup
  timeline: MatrixEvent[];
  unread: number;
}

export interface Store {
  rooms: Map<string, Room>;
  version: number; // bumped on every change; components re-render off this
  syncState: "starting" | "live" | "error";
  lastError?: string;
}

export const store: Store = {
  rooms: new Map(),
  version: 0,
  syncState: "starting",
};

type Listener = () => void;
const listeners = new Set<Listener>();

export function subscribe(fn: Listener): () => void {
  listeners.add(fn);
  return () => listeners.delete(fn);
}

function bump(): void {
  store.version++;
  listeners.forEach((fn) => fn());
}

/** Optimistic local echo: append a pending event immediately so the sender
 * sees their message before the server round-trips. The real event arrives
 * via /sync and is de-duped by the txn-id we stamp into the local event_id. */
export function addLocalEcho(roomId: string, ev: MatrixEvent): void {
  const room = store.rooms.get(roomId);
  if (!room) return;
  room.timeline.push(ev);
  bump();
}

/** Mark a previously-echoed event as failed-to-send (red, retryable). */
export function markEchoFailed(roomId: string, eventId: string): void {
  const room = store.rooms.get(roomId);
  const ev = room?.timeline.find((e) => e.event_id === eventId);
  if (ev) {
    ev.content = { ...ev.content, _failed: true };
    bump();
  }
}

/** A real, human room name from m.room.name / canonical_alias, or "" if none. */
function explicitName(state: MatrixEvent[], timeline: MatrixEvent[]): string {
  const all = [...state, ...timeline];
  const nameEv = all.findLast((e) => e.type === "m.room.name");
  if (nameEv?.content?.name) return nameEv.content.name as string;
  const aliasEv = all.findLast((e) => e.type === "m.room.canonical_alias");
  if (aliasEv?.content?.alias) return (aliasEv.content.alias as string).split(":")[0].slice(1);
  return "";
}

/** For a DM, the OTHER person's display name (not me) from m.room.member events. */
function otherMemberName(events: MatrixEvent[], myUserId: string): string {
  const members = events.filter((e) => e.type === "m.room.member" && e.state_key && e.state_key !== myUserId);
  const joined = members.find((e) => e.content?.membership === "join") ?? members[0];
  if (!joined) return "";
  return (joined.content?.displayname as string) || joined.state_key!.split(":")[0].replace(/^@/, "");
}

function applySync(resp: SyncResponse, myUserId: string): void {
  const joined = resp.rooms?.join ?? {};
  for (const [roomId, data] of Object.entries(joined)) {
    const existing = store.rooms.get(roomId);
    const stateEvents = data.state?.events ?? [];
    const allState = [...stateEvents, ...(data.timeline?.events ?? [])];
    const newEvents = (data.timeline?.events ?? []).filter(
      (e) => e.type === "m.room.message" || e.type === "com.fleet.gen.request",
    );
    const named = explicitName(stateEvents, data.timeline?.events ?? []);
    const isDm = !named;

    if (existing) {
      const incomingTxns = new Set(
        newEvents.map((e) => e.unsigned?.transaction_id).filter(Boolean),
      );
      if (incomingTxns.size) {
        existing.timeline = existing.timeline.filter(
          (e) => !(e.event_id.startsWith("echo:") && incomingTxns.has(e.event_id.slice(5))),
        );
      }
      const seen = new Set(existing.timeline.map((e) => e.event_id));
      existing.timeline.push(...newEvents.filter((e) => !seen.has(e.event_id)));
      existing.unread = data.unread_notifications?.notification_count ?? existing.unread;
      if (named) existing.name = named;
      else {
        const dm = otherMemberName(allState, myUserId);
        if (dm) { existing.name = dm; existing.needsName = false; }
      }
    } else {
      let name = named;
      let needsName = false;
      if (!name) {
        name = otherMemberName(allState, myUserId);
        if (!name) { name = "Direct message"; needsName = true; } // resolved via API below
      }
      store.rooms.set(roomId, { id: roomId, name, isDm, needsName, timeline: newEvents, unread: data.unread_notifications?.notification_count ?? 0 });
    }
  }
  bump();
}

/** For DM rooms we couldn't name from sync state, fetch joined_members and
 * name them after the other party. One-shot, after the initial sync. */
async function resolveDmNames(token: string, myUserId: string): Promise<void> {
  const pending = [...store.rooms.values()].filter((r) => r.needsName);
  for (const room of pending) {
    try {
      const members = await joinedMembers(token, room.id);
      const other = members.find((m) => m.userId !== myUserId) ?? members[0];
      if (other) { room.name = other.displayName; room.needsName = false; }
    } catch { /* leave as-is */ }
  }
  if (pending.length) bump();
}

let abort: AbortController | null = null;

/** The forever sync loop. Call once after login; cancels on logout.
 * ALWAYS starts with a full snapshot (since=null) so the complete room list
 * populates the sidebar — a persisted `since` would give an incremental sync
 * that only returns rooms with new activity, leaving the sidebar empty on
 * reload. We use next_batch for incremental polling only AFTER the snapshot. */
export async function startSyncLoop(session: Session): Promise<void> {
  abort?.abort();
  abort = new AbortController();
  let since: string | null = null; // force a full initial sync, ignore stale token
  for (;;) {
    if (abort.signal.aborted) return;
    try {
      const firstSync = since === null;
      const resp = await sync(session.token, since, abort.signal);
      since = resp.next_batch;
      saveSince(since);
      applySync(resp, session.userId);
      if (store.syncState !== "live") {
        store.syncState = "live";
        bump();
      }
      if (firstSync) resolveDmNames(session.token, session.userId); // name DMs after the other party
    } catch (e: any) {
      if (abort.signal.aborted) return;
      store.syncState = "error";
      store.lastError = e?.message ?? String(e);
      bump();
      await new Promise((r) => setTimeout(r, 3000)); // backoff then retry
    }
  }
}

export function stopSyncLoop(): void {
  abort?.abort();
}
