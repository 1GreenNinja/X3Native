// MemberPanel.tsx — the right-hand member list with presence (PR-6).
// Fetches joined members for the active room + their presence, refreshing
// when the room changes. Toggled from the room header.

import { useEffect, useState } from "preact/hooks";
import {
  type Session,
  type RoomMember,
  joinedMembers,
  getPresence,
} from "../client";

function avatarColor(sender: string): string {
  let h = 0;
  for (let i = 0; i < sender.length; i++) h = (h * 31 + sender.charCodeAt(i)) % 360;
  return `hsl(${h}, 45%, 45%)`;
}

interface MemberRow extends RoomMember {
  presence: string;
}

export function MemberPanel({ session, roomId }: { session: Session; roomId: string }) {
  const [members, setMembers] = useState<MemberRow[]>([]);
  const [loading, setLoading] = useState(true);

  useEffect(() => {
    let cancelled = false;
    setLoading(true);
    (async () => {
      const base = await joinedMembers(session.token, roomId);
      if (cancelled) return;
      // Show names immediately, then fill presence as it resolves.
      setMembers(base.map((m) => ({ ...m, presence: "offline" })));
      setLoading(false);
      const withPresence = await Promise.all(
        base.map(async (m) => ({
          ...m,
          presence: (await getPresence(session.token, m.userId)).presence,
        })),
      );
      if (!cancelled) setMembers(withPresence);
    })();
    return () => {
      cancelled = true;
    };
  }, [roomId]);

  const online = members.filter((m) => m.presence === "online");
  const away = members.filter((m) => m.presence === "unavailable");
  const offline = members.filter((m) => m.presence !== "online" && m.presence !== "unavailable");

  const section = (label: string, rows: MemberRow[]) =>
    rows.length > 0 && (
      <>
        <div class="member-section">{label} — {rows.length}</div>
        {rows.map((m) => (
          <div class="member-row" key={m.userId} title={m.userId}>
            <span class="member-avatar" style={{ background: avatarColor(m.userId) }}>
              {m.displayName.slice(0, 2).toUpperCase()}
            </span>
            <span class={`presence-pip presence-${m.presence}`} />
            <span class="member-name">{m.displayName}</span>
          </div>
        ))}
      </>
    );

  return (
    <aside class="member-panel">
      <header class="member-header">Members{members.length ? ` · ${members.length}` : ""}</header>
      <div class="member-list">
        {loading && <div class="member-loading">Loading…</div>}
        {section("Online", online)}
        {section("Away", away)}
        {section("Offline", offline)}
      </div>
    </aside>
  );
}
