// store.ts — Slick's whole state model: a room map fed by one /sync loop.
// No state library; Preact components subscribe via a version counter.

import {
  type MatrixEvent,
  type Session,
  type SyncResponse,
  sync,
  loadSince,
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

/** The forever sync loop. Call once after login; cancels on logout. */
export async function startSyncLoop(session: Session): Promise<void> {
  abort?.abort();
  abort = new AbortController();
  let since = loadSince();
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
