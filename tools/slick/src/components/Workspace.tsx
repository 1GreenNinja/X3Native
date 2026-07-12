import { useEffect, useState } from "preact/hooks";
import { type Session, type RoomMember, createChannel, createDm, fleetRoster } from "../client";
import { store, subscribe, startSyncLoop, findDmWith, type Room } from "../store";
import {
  type CatConfig, loadCats, saveCats, addSection, removeSection, assignRoom, toggleCollapsed, toggleStar,
} from "../categories";
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
  const [cats, setCatsState] = useState<CatConfig>(loadCats());
  const [addingSection, setAddingSection] = useState(false);
  const [sectionName, setSectionName] = useState("");
  const [dragId, setDragId] = useState<string | null>(null);
  const setCats = (c: CatConfig) => { setCatsState(c); saveCats(c); };
  const { channels, dms } = sortedRooms();

  // Starred channels pin to the top section and drop out of their normal group
  const isStarred = (id: string) => cats.starred.includes(id);
  const starredChannels = channels.filter((r) => isStarred(r.id));
  const inSection = (sec: string) => channels.filter((r) => !isStarred(r.id) && cats.assign[r.id] === sec);
  const uncategorized = channels.filter((r) => {
    if (isStarred(r.id)) return false;
    const a = cats.assign[r.id];
    return !a || !cats.sections.includes(a);
  });
  const [openStar, setOpenStar] = useState(true);
  const [openPeople, setOpenPeople] = useState(true);
  const [roster, setRoster] = useState<RoomMember[]>([]);

  // Fleet roster — union of members across all your channels, refreshed when the
  // channel list changes. This is the Slack-style "People" list to DM anyone.
  useEffect(() => {
    if (!channels.length) return;
    let cancelled = false;
    fleetRoster(session.token, channels.map((c) => c.id), session.userId)
      .then((r) => { if (!cancelled) setRoster(r); })
      .catch(() => { /* leave prior roster */ });
    return () => { cancelled = true; };
  }, [channels.length]);

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
  const [mobileRoom, setMobileRoom] = useState(false); // mobile: viewing a room vs the list

  // Auto-select the first channel once sync lands (desktop only — on mobile
  // we want the channel list first, not an auto-opened room)
  useEffect(() => {
    if (!activeId && channels.length && window.innerWidth > 640) setActiveId(channels[0].id);
  }, [channels.length]);

  const openRoom = (id: string) => { setActiveId(id); setMobileRoom(true); };

  // Click a person in the roster: open the existing DM if there is one, else
  // start a fresh DM and select it once /sync picks it up.
  const openPerson = async (m: RoomMember) => {
    const existing = findDmWith(m.userId);
    if (existing) { openRoom(existing.id); return; }
    try {
      const id = await createDm(session.token, m.userId);
      startSyncLoop(session);
      setTimeout(() => openRoom(id), 1000);
    } catch { /* surfaced via sync */ }
  };

  return (
    <div class={`workspace ${showMembers && active ? "with-members" : ""} ${mobileRoom ? "mobile-room" : ""}`}>
      <aside class="sidebar">
        <header class="ws-header">
          <span class="ws-name">FleetCommand</span>
          <span class={`sync-dot sync-${store.syncState}`} title={store.syncState} />
        </header>
        <nav>
          {/* draggable channel row */}
          {(() => {
            const channelRow = (r: Room) => (
              <a key={r.id} draggable
                class={`room-row ${r.id === activeId ? "active" : ""} ${r.unread ? "unread" : ""}`}
                onClick={() => openRoom(r.id)}
                onDragStart={() => setDragId(r.id)}
                onDragEnd={() => setDragId(null)}>
                <span class="hash">#</span> <span class="room-name">{r.name}</span>
                {r.unread > 0 && <span class="badge">{r.unread}</span>}
                <button class={`star ${isStarred(r.id) ? "on" : ""}`} title="Star"
                  onClick={(e) => { e.stopPropagation(); setCats(toggleStar(cats, r.id)); }}>
                  {isStarred(r.id) ? "★" : "☆"}
                </button>
              </a>
            );
            const dropProps = (section: string | null) => ({
              onDragOver: (e: DragEvent) => { e.preventDefault(); (e.currentTarget as HTMLElement).classList.add("drop-hot"); },
              onDragLeave: (e: DragEvent) => (e.currentTarget as HTMLElement).classList.remove("drop-hot"),
              onDrop: (e: DragEvent) => {
                e.preventDefault(); (e.currentTarget as HTMLElement).classList.remove("drop-hot");
                if (dragId) setCats(assignRoom(cats, dragId, section));
                setDragId(null);
              },
            });
            return (
              <>
                {/* Starred — pinned channels at the very top (Slack) */}
                {starredChannels.length > 0 && (
                  <>
                    <div class="section-head">
                      <button class="section-toggle" onClick={() => setOpenStar((v) => !v)}>
                        <span class="caret">{openStar ? "▾" : "▸"}</span> ★ Starred
                      </button>
                    </div>
                    {openStar && starredChannels.map(channelRow)}
                  </>
                )}

                {/* custom project sections */}
                {cats.sections.map((sec) => {
                  const open = !cats.collapsed[sec];
                  return (
                    <div key={sec} class="cat-section" {...dropProps(sec)}>
                      <div class="section-head">
                        <button class="section-toggle" onClick={() => setCats(toggleCollapsed(cats, sec))}>
                          <span class="caret">{open ? "▾" : "▸"}</span> {sec}
                        </button>
                        <button class="section-add" title="Remove section (rooms move back to Channels)"
                          onClick={() => setCats(removeSection(cats, sec))}>✕</button>
                      </div>
                      {open && inSection(sec).map(channelRow)}
                      {open && inSection(sec).length === 0 && <div class="section-empty">drag a channel here</div>}
                    </div>
                  );
                })}

                {/* Channels (uncategorized) — also a drop target to un-assign */}
                <div class="cat-section" {...dropProps(null)}>
                  <div class="section-head">
                    <button class="section-toggle" onClick={() => setOpenCh((v) => !v)}>
                      <span class="caret">{openCh ? "▾" : "▸"}</span> Channels
                      {uncategorized.length > 0 && <span class="sec-count">{uncategorized.length}</span>}
                    </button>
                    <span>
                      <button class="section-add" title="New section" onClick={() => setAddingSection(true)}>📁</button>
                      <button class="section-add" title="Create channel" onClick={() => setCreating(true)}>＋</button>
                    </span>
                  </div>
                  {addingSection && (
                    <div class="create-row">
                      <input class="create-input" placeholder="section name (e.g. EFLZ)" value={sectionName} autofocus
                        onInput={(e) => setSectionName((e.target as HTMLInputElement).value)}
                        onKeyDown={(e) => {
                          if (e.key === "Enter") { setCats(addSection(cats, sectionName)); setSectionName(""); setAddingSection(false); }
                          if (e.key === "Escape") { setSectionName(""); setAddingSection(false); }
                        }}
                        onBlur={() => { if (!sectionName.trim()) setAddingSection(false); }} />
                    </div>
                  )}
                  {creating && (
                    <div class="create-row">
                      <span class="hash">#</span>
                      <input class="create-input" placeholder="new-channel-name" value={newName} autofocus
                        onInput={(e) => setNewName((e.target as HTMLInputElement).value)}
                        onKeyDown={(e) => { if (e.key === "Enter") createRoom(); if (e.key === "Escape") { setCreating(false); setNewName(""); } }}
                        onBlur={() => { if (!newName.trim()) setCreating(false); }} />
                    </div>
                  )}
                  {openCh && uncategorized.map(channelRow)}
                  {openCh && uncategorized.length === 0 && !creating && (
                    <div class="section-empty">{store.syncState === "live" ? "No channels here" : "Syncing…"}</div>
                  )}
                </div>

                {/* Direct messages */}
                <div class="section-head">
                  <button class="section-toggle" onClick={() => setOpenDm((v) => !v)}>
                    <span class="caret">{openDm ? "▾" : "▸"}</span> Direct messages
                    {dms.length > 0 && <span class="sec-count">{dms.length}</span>}
                  </button>
                </div>
                {openDm && dms.map((r) => (
                  <a key={r.id}
                    class={`room-row ${r.id === activeId ? "active" : ""} ${r.unread ? "unread" : ""}`}
                    onClick={() => openRoom(r.id)}>
                    <span class="presence-dot" /> {r.name}
                    {r.unread > 0 && <span class="badge">{r.unread}</span>}
                  </a>
                ))}
                {openDm && dms.length === 0 && (
                  <div class="section-empty">{store.syncState === "live" ? "No DMs yet" : "Syncing…"}</div>
                )}

                {/* People — the full fleet roster; click anyone to DM them (Slack-style) */}
                <div class="section-head">
                  <button class="section-toggle" onClick={() => setOpenPeople((v) => !v)}>
                    <span class="caret">{openPeople ? "▾" : "▸"}</span> People
                    {roster.length > 0 && <span class="sec-count">{roster.length}</span>}
                  </button>
                </div>
                {openPeople && roster.map((m) => (
                  <a key={m.userId} class="room-row" title={m.userId} onClick={() => openPerson(m)}>
                    <span class="presence-dot" /> {m.displayName}
                  </a>
                ))}
                {openPeople && roster.length === 0 && (
                  <div class="section-empty">{store.syncState === "live" ? "No members" : "Syncing…"}</div>
                )}
              </>
            );
          })()}
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
              <button class="mobile-back" onClick={() => setMobileRoom(false)} title="Back to channels">‹</button>
              <span class="room-title"><span class="hash">{active.isDm ? "@" : "#"}</span> {active.name}</span>
              <button
                class={`members-toggle ${showMembers ? "on" : ""}`}
                onClick={() => setShowMembers((s) => !s)}
                title="Toggle members"
              >
                👥
              </button>
            </header>
            <MessageStream room={active} session={session} />
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
