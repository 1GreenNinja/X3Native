import { useEffect, useState } from "preact/hooks";
import type { Session } from "../client";
import { store, subscribe, type Room } from "../store";
import { MessageStream } from "./MessageStream";
import { Composer } from "./Composer";
import { MemberPanel } from "./MemberPanel";
import { Settings } from "./Settings";

function useStore(): number {
  const [v, setV] = useState(store.version);
  useEffect(() => subscribe(() => setV(store.version)), []);
  return v;
}

function sortedRooms(): { channels: Room[]; dms: Room[] } {
  const all = [...store.rooms.values()];
  const channels = all.filter((r) => !r.isDm).sort((a, b) => a.name.localeCompare(b.name));
  const dms = all.filter((r) => r.isDm).sort((a, b) => a.name.localeCompare(b.name));
  return { channels, dms };
}

export function Workspace({ session, onLogout }: { session: Session; onLogout: () => void }) {
  useStore();
  const [activeId, setActiveId] = useState<string | null>(null);
  const [showMembers, setShowMembers] = useState(false);
  const [showSettings, setShowSettings] = useState(false);
  const { channels, dms } = sortedRooms();
  const active = activeId ? store.rooms.get(activeId) : null;

  // Auto-select the first channel once sync lands
  useEffect(() => {
    if (!activeId && channels.length) setActiveId(channels[0].id);
  }, [channels.length]);

  return (
    <div class={`workspace ${showMembers && active ? "with-members" : ""}`}>
      <aside class="sidebar">
        <header class="ws-header">
          <span class="ws-name">FleetCommand</span>
          <span class={`sync-dot sync-${store.syncState}`} title={store.syncState} />
        </header>
        <nav>
          <div class="section-label">Channels</div>
          {channels.map((r) => (
            <a
              key={r.id}
              class={`room-row ${r.id === activeId ? "active" : ""} ${r.unread ? "unread" : ""}`}
              onClick={() => setActiveId(r.id)}
            >
              <span class="hash">#</span> {r.name}
              {r.unread > 0 && <span class="badge">{r.unread}</span>}
            </a>
          ))}
          <div class="section-label">Direct messages</div>
          {dms.map((r) => (
            <a
              key={r.id}
              class={`room-row ${r.id === activeId ? "active" : ""}`}
              onClick={() => setActiveId(r.id)}
            >
              <span class="presence-dot" /> {r.name}
            </a>
          ))}
        </nav>
        <footer class="me">
          <span class="me-id">{session.userId.split(":")[0]}</span>
          <span class="me-actions">
            <button class="gear" onClick={() => setShowSettings(true)} title="Settings">⚙️</button>
            <button class="logout" onClick={onLogout}>Sign out</button>
          </span>
        </footer>
      </aside>

      <main class="room-view">
        {active ? (
          <>
            <header class="room-header">
              <span class="room-title"><span class="hash">#</span> {active.name}</span>
              <button
                class={`members-toggle ${showMembers ? "on" : ""}`}
                onClick={() => setShowMembers((s) => !s)}
                title="Toggle members"
              >
                👥
              </button>
            </header>
            <MessageStream room={active} />
            <Composer session={session} roomId={active.id} roomName={active.name} />
          </>
        ) : (
          <div class="stream-empty">
            {store.syncState === "starting" ? "Syncing…" : "Pick a channel"}
          </div>
        )}
      </main>

      {showMembers && active && <MemberPanel session={session} roomId={active.id} />}
      {showSettings && <Settings session={session} onClose={() => setShowSettings(false)} />}
    </div>
  );
}
