// store.ts — Slick's whole state model: a room map fed by one /sync loop.
// No state library; Preact components subscribe via a version counter.

import {
  type MatrixEvent,
  type Session,
  type SyncResponse,
  sync,
  saveSince,
} from "./client";

export interface Room {
  id: string;
  name: string;
  isDm: boolean;
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

function roomNameFromState(roomId: string, state: MatrixEvent[], timeline: MatrixEvent[]): string {
  const all = [...state, ...timeline];
  const nameEv = all.findLast((e) => e.type === "m.room.name");
  if (nameEv?.content?.name) return nameEv.content.name;
  const aliasEv = all.findLast((e) => e.type === "m.room.canonical_alias");
  if (aliasEv?.content?.alias) return (aliasEv.content.alias as string).split(":")[0].slice(1);
  return roomId.slice(1, 12);
}

function isDmFromState(state: MatrixEvent[], timeline: MatrixEvent[]): boolean {
  // Heuristic good enough for the fleet: a room with no m.room.name state is a DM.
  return ![...state, ...timeline].some((e) => e.type === "m.room.name" && e.content?.name);
}

function applySync(resp: SyncResponse): void {
  const joined = resp.rooms?.join ?? {};
  for (const [roomId, data] of Object.entries(joined)) {
    const existing = store.rooms.get(roomId);
    const stateEvents = data.state?.events ?? [];
    const newEvents = (data.timeline?.events ?? []).filter(
      (e) => e.type === "m.room.message" || e.type === "com.fleet.gen.request",
    );
    if (existing) {
      // Drop any local echo whose txn id matches an incoming real event, so
      // the optimistic message isn't shown twice (echo id == "echo:<txn>").
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
      if (stateEvents.length) {
        existing.name = roomNameFromState(roomId, stateEvents, existing.timeline);
      }
    } else {
      store.rooms.set(roomId, {
        id: roomId,
        name: roomNameFromState(roomId, stateEvents, data.timeline?.events ?? []),
        isDm: isDmFromState(stateEvents, data.timeline?.events ?? []),
        timeline: newEvents,
        unread: data.unread_notifications?.notification_count ?? 0,
      });
    }
  }
  bump();
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
      const resp = await sync(session.token, since, abort.signal);
      since = resp.next_batch;
      saveSince(since);
      applySync(resp);
      if (store.syncState !== "live") {
        store.syncState = "live";
        bump();
      }
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
