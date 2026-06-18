// categories.ts — client-side custom sidebar sections (Slack "sections").
// Group project rooms into named, collapsible categories. Stored locally per
// user; Matrix has no native concept for this so it lives in localStorage.

export interface CatConfig {
  sections: string[];                 // ordered custom section names
  assign: Record<string, string>;     // roomId -> section name
  collapsed: Record<string, boolean>; // section name -> collapsed?
  starred: string[];                  // roomIds pinned to the Starred section
}

const KEY = "slick.categories";

export function loadCats(): CatConfig {
  try {
    const raw = localStorage.getItem(KEY);
    if (raw) return { sections: [], assign: {}, collapsed: {}, starred: [], ...JSON.parse(raw) };
  } catch { /* fall through */ }
  return { sections: [], assign: {}, collapsed: {}, starred: [] };
}

/** Toggle a room's Starred pin. Starred rooms surface in the top section. */
export function toggleStar(c: CatConfig, roomId: string): CatConfig {
  const starred = c.starred.includes(roomId)
    ? c.starred.filter((id) => id !== roomId)
    : [...c.starred, roomId];
  return { ...c, starred };
}

export function saveCats(c: CatConfig): void {
  localStorage.setItem(KEY, JSON.stringify(c));
}

export function addSection(c: CatConfig, name: string): CatConfig {
  const n = name.trim();
  if (!n || c.sections.includes(n)) return c;
  return { ...c, sections: [...c.sections, n] };
}

export function removeSection(c: CatConfig, name: string): CatConfig {
  const assign = { ...c.assign };
  for (const id of Object.keys(assign)) if (assign[id] === name) delete assign[id];
  return { ...c, sections: c.sections.filter((s) => s !== name), assign };
}

/** Move a room into a section, or pass null to send it back to "Channels". */
export function assignRoom(c: CatConfig, roomId: string, section: string | null): CatConfig {
  const assign = { ...c.assign };
  if (section) assign[roomId] = section;
  else delete assign[roomId];
  return { ...c, assign };
}

export function toggleCollapsed(c: CatConfig, name: string): CatConfig {
  return { ...c, collapsed: { ...c.collapsed, [name]: !c.collapsed[name] } };
}
