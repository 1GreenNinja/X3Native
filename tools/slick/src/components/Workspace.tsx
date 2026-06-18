import { useEffect, useState } from "preact/hooks";
import { type Session, createChannel } from "../client";
import { store, subscribe, startSyncLoop, type Room } from "../store";
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
  const [openCh, setOpenCh] = useState(true);
  const [openDm, setOpenDm] = useState(true);
  const [creating, setCreating] = useState(false);
  const [newName, setNewName] = useState("");
  const { channels, dms } = sortedRooms();

  const createRoom = async () => {
    const name = newName.trim();
    if (!name) { setCreating(false); return; }
    setCreating(false); setNewName("");
    try {
      const id = await createChannel(session.token, name);
      startSyncLoop(session);      // refresh so the new room appears in the tree
      setTimeout(() => setActiveId(id), 1200);
    } catch { /* surfaced via sync */ }
  };
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
          <div class="section-head">
            <button class="section-toggle" onClick={() => setOpenCh((v) => !v)}>
              <span class="caret">{openCh ? "▾" : "▸"}</span> Channels
            </button>
            <button class="section-add" title="Create channel" onClick={() => setCreating(true)}>＋</button>
          </div>
          {creating && (
            <div class="create-row">
              <span class="hash">#</span>
              <input
                class="create-input" placeholder="new-channel-name" value={newName} autofocus
                onInput={(e) => setNewName((e.target as HTMLInputElement).value)}
                onKeyDown={(e) => { if (e.key === "Enter") createRoom(); if (e.key === "Escape") { setCreating(false); setNewName(""); } }}
                onBlur={() => { if (!newName.trim()) setCreating(false); }}
              />
            </div>
          )}
          {openCh && channels.map((r) => (
            <a key={r.id}
              class={`room-row ${r.id === activeId ? "active" : ""} ${r.unread ? "unread" : ""}`}
              onClick={() => setActiveId(r.id)}>
              <span class="hash">#</span> {r.name}
              {r.unread > 0 && <span class="badge">{r.unread}</span>}
            </a>
          ))}
          {openCh && channels.length === 0 && !creating && (
            <div class="section-empty">{store.syncState === "live" ? "No channels yet" : "Syncing…"}</div>
          )}

          <div class="section-head">
            <button class="section-toggle" onClick={() => setOpenDm((v) => !v)}>
              <span class="caret">{openDm ? "▾" : "▸"}</span> Direct messages
            </button>
          </div>
          {openDm && dms.map((r) => (
            <a key={r.id}
              class={`room-row ${r.id === activeId ? "active" : ""} ${r.unread ? "unread" : ""}`}
              onClick={() => setActiveId(r.id)}>
              <span class="presence-dot" /> {r.name}
              {r.unread > 0 && <span class="badge">{r.unread}</span>}
            </a>
          ))}
          {openDm && dms.length === 0 && (
            <div class="section-empty">{store.syncState === "live" ? "No DMs yet" : "Syncing…"}</div>
          )}
        </nav>
        <footer class="me">
          <span class="me-id">{session.userId.split(":")[0]}</span>
          <span class="me-actions">
            <button class="gear" onClick={() => startSyncLoop(session)} title="Refresh">🔄</button>
            <button class="gear" onClick={() => setShowSettings(true)} title="Settings">⚙️</button>
            <button class="logout" onClick={onLogout}>Sign out</button>
          </span>
        </footer>
      </aside>

      <main class="room-view">
        {active ? (
          <>
            <header class="room-header">
              <span class="room-title"><span class="hash">{active.isDm ? "@" : "#"}</span> {active.name}</span>
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
