// MessageStream.tsx — the grouped, day-divided message list (PR-2).
// Slack-style: consecutive messages from the same sender within 5 minutes
// collapse under one sender header; day dividers separate calendar days.

import { useEffect, useRef } from "preact/hooks";
import type { MatrixEvent, Session } from "../client";
import { backfillRoom, type Room } from "../store";
import { ImageEvent } from "./ImageEvent";

const GROUP_WINDOW_MS = 5 * 60 * 1000;

function senderName(id: string): string {
  return id.split(":")[0].replace(/^@/, "");
}

function dayLabel(ts: number): string {
  const d = new Date(ts);
  const today = new Date();
  const yesterday = new Date();
  yesterday.setDate(today.getDate() - 1);
  const sameDay = (a: Date, b: Date) =>
    a.getFullYear() === b.getFullYear() &&
    a.getMonth() === b.getMonth() &&
    a.getDate() === b.getDate();
  if (sameDay(d, today)) return "Today";
  if (sameDay(d, yesterday)) return "Yesterday";
  return d.toLocaleDateString([], { weekday: "long", month: "long", day: "numeric" });
}

function clockTime(ts: number): string {
  return new Date(ts).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
}

/** Render a single event's body by msgtype. m.image lands fully in PR-4;
 * here we show a typed placeholder chip so images are visible-as-present. */
function EventBody({ ev }: { ev: MatrixEvent }) {
  const c = ev.content ?? {};
  const stateClass = c._failed ? " failed" : c._pending ? " pending" : "";
  const suffix = c._failed ? " ⚠ not sent" : "";
  // fleet.gen request (StarForge's contract): a normal m.room.message carrying
  // structured gen fields. Render as a cockpit chip showing the job spec.
  const gen = c["fleet.gen"];
  if (gen) {
    const tgt = gen.target && gen.target !== "any" ? ` → ${gen.target}` : "";
    const status = c._failed ? " ⚠ not sent" : c._pending ? " (queued…)" : " (dispatched)";
    return (
      <div class={`msg-body gen-chip${stateClass}`}>
        ⚙️ <b>/gen</b> {gen.mode === "mesh" ? "🧊" : "🖼️"} {gen.prompt}
        <em> · {gen.size}px · {gen.steps} steps{tgt}{status}</em>
      </div>
    );
  }
  if (c.msgtype === "m.image") {
    if (c._pending) {
      return <div class={`msg-body image-chip${stateClass}`}>🖼️ {c.body ?? "image"} <em>(uploading…){suffix}</em></div>;
    }
    return <div class={`msg-body${stateClass}`}><ImageEvent content={c} /></div>;
  }
  if (c.msgtype === "m.notice") {
    return <div class={`msg-body notice${stateClass}`}>{c.body}{suffix}</div>;
  }
  if (c.msgtype === "m.emote") {
    return <div class={`msg-body emote${stateClass}`}><em>* {senderName(ev.sender)} {c.body}</em>{suffix}</div>;
  }
  return <div class={`msg-body${stateClass}`}>{c.body ?? `[${ev.type}]`}{suffix}</div>;
}

interface Group {
  sender: string;
  startTs: number;
  events: MatrixEvent[];
}

function groupEvents(events: MatrixEvent[]): { day: string; groups: Group[] }[] {
  const days: { day: string; groups: Group[] }[] = [];
  let curDay: { day: string; groups: Group[] } | null = null;
  let curGroup: Group | null = null;

  for (const ev of events) {
    const day = dayLabel(ev.origin_server_ts);
    if (!curDay || curDay.day !== day) {
      curDay = { day, groups: [] };
      days.push(curDay);
      curGroup = null;
    }
    const sameSender = curGroup && curGroup.sender === ev.sender;
    const withinWindow =
      curGroup && ev.origin_server_ts - curGroup.events[curGroup.events.length - 1].origin_server_ts < GROUP_WINDOW_MS;
    if (sameSender && withinWindow) {
      curGroup!.events.push(ev);
    } else {
      curGroup = { sender: ev.sender, startTs: ev.origin_server_ts, events: [ev] };
      curDay.groups.push(curGroup);
    }
  }
  return days;
}

function avatarColor(sender: string): string {
  // deterministic hue from the sender string
  let h = 0;
  for (let i = 0; i < sender.length; i++) h = (h * 31 + sender.charCodeAt(i)) % 360;
  return `hsl(${h}, 45%, 45%)`;
}

export function MessageStream({ room, session }: { room: Room; session: Session }) {
  const endRef = useRef<HTMLDivElement>(null);
  const events = room.timeline; // FULL history — backfilled to the room's creation
  const days = groupEvents(events);

  // Backfill the whole history the first time a room is opened.
  useEffect(() => {
    void backfillRoom(session, room.id);
  }, [room.id]);

  // Auto-scroll to bottom only when a NEW (latest) message arrives — not while
  // older history streams in above (which would otherwise yank the view down).
  const lastId = events.length ? events[events.length - 1].event_id : "";
  useEffect(() => {
    endRef.current?.scrollIntoView({ block: "end" });
  }, [lastId, room.id]);

  if (!events.length) {
    return (
      <div class="stream-empty">
        {room.backfilling ? "Loading history…" : "No messages here yet."}
      </div>
    );
  }

  return (
    <div class="stream">
      {room.backfilling && <div class="day-divider"><span>loading older history…</span></div>}
      {room.hitStart && <div class="day-divider"><span>· beginning of {room.isDm ? "conversation" : "channel"} ·</span></div>}
      {days.map((d) => (
        <div key={d.day}>
          <div class="day-divider"><span>{d.day}</span></div>
          {d.groups.map((g) => (
            <div class="msg-group" key={g.events[0].event_id}>
              <div class="avatar" style={{ background: avatarColor(g.sender) }}>
                {senderName(g.sender).slice(0, 2).toUpperCase()}
              </div>
              <div class="msg-group-body">
                <div class="msg-group-head">
                  <span class="msg-sender">{senderName(g.sender)}</span>
                  <span class="msg-time">{clockTime(g.startTs)}</span>
                </div>
                {g.events.map((ev) => (
                  <EventBody key={ev.event_id} ev={ev} />
                ))}
              </div>
            </div>
          ))}
        </div>
      ))}
      <div ref={endRef} />
    </div>
  );
}
