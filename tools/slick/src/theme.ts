// theme.ts — user-tunable look (Appearance settings). Writes CSS custom
// properties on :root so the whole glassy skin retints/reglazes live, and
// persists choices in localStorage so they survive reloads.

export interface Theme {
  accent: string;   // hex, drives --cyan (links, buttons, glows, active rail)
  glass: number;    // 0..1 FROST level — drives backdrop blur (0=clear, 1=heavy frost)
  grid: boolean;    // faint plating grid behind everything
  scanlines: boolean; // ctOS CRT scanline + flicker overlay
  glow: boolean;      // message-row hover edge glow
  motion: boolean;    // message entrance + UI micro-animations
  splash: boolean;    // ctOS boot handshake on launch
  shine: boolean;     // glossy specular highlight + sheen on glass panels
}

const FX = { grid: true, scanlines: true, glow: true, motion: true, splash: true, shine: true };

export const THEME_PRESETS: Record<string, Theme> = {
  // the shipped gunmetal + cyan
  "Fleet Cyan":     { accent: "#3fd0e0", glass: 0.62, ...FX },
  // Deus Ex black-and-gold cyber-renaissance
  "Augmented Gold": { accent: "#d9a441", glass: 0.55, ...FX },
  // Watch_Dogs / ctOS green terminal
  "ctOS Green":     { accent: "#46e08a", glass: 0.58, ...FX },
  // Cyberpunk hot magenta
  "Night City":     { accent: "#ff3d7f", glass: 0.5,  ...FX },
};

const THEME_KEY = "slick.theme";
const DEFAULT: Theme = THEME_PRESETS["Fleet Cyan"];

export function getTheme(): Theme {
  try {
    const raw = localStorage.getItem(THEME_KEY);
    return raw ? { ...DEFAULT, ...JSON.parse(raw) } : { ...DEFAULT };
  } catch {
    return { ...DEFAULT };
  }
}

/** Parse #rrggbb -> {r,g,b}. */
function hexRgb(hex: string): { r: number; g: number; b: number } {
  const m = /^#?([0-9a-f]{6})$/i.exec(hex.trim());
  const n = m ? parseInt(m[1], 16) : 0x3fd0e0;
  return { r: (n >> 16) & 255, g: (n >> 8) & 255, b: n & 255 };
}

/** Apply a theme to :root by overriding the CSS custom properties. */
export function applyTheme(t: Theme): void {
  const root = document.documentElement.style;
  const { r, g, b } = hexRgb(t.accent);
  root.setProperty("--cyan", t.accent);
  root.setProperty("--cyan-glow", `0 0 12px rgba(${r}, ${g}, ${b}, 0.45)`);
  // a dimmer companion for gradients/buttons
  root.setProperty("--cyan-dim", `rgb(${Math.round(r * 0.62)}, ${Math.round(g * 0.62)}, ${Math.round(b * 0.62)})`);
  // FROST: the slider now drives the backdrop blur (0 = clear glass, 1 = heavy
  // frost). Panel tint opacity stays at a fixed glassy value so panels read as
  // glass regardless of frost. More frost also = slightly more tint.
  const blurPx = Math.round(t.glass * 30);
  root.setProperty("--glass-blur", `${blurPx}px`);
  const tint = 0.34 + t.glass * 0.30;            // 0.34 clear → 0.64 frosted
  root.setProperty("--glass", `rgba(26, 33, 46, ${tint.toFixed(2)})`);
  root.setProperty("--glass-light", `rgba(40, 50, 68, ${Math.max(0, tint - 0.12).toFixed(2)})`);
  // plating grid on/off
  root.setProperty("--grid-alpha", t.grid ? "0.035" : "0");
  // FX toggles ride as classes on <html> so CSS can gate them
  const cl = document.documentElement.classList;
  cl.toggle("fx-scanlines", t.scanlines);
  cl.toggle("fx-glow", t.glow);
  cl.toggle("fx-motion", t.motion);
  cl.toggle("fx-shine", t.shine);
}

export function setTheme(t: Theme): void {
  localStorage.setItem(THEME_KEY, JSON.stringify(t));
  applyTheme(t);
}

/** Call once at startup so a saved theme is live before first paint. */
export function initTheme(): void {
  applyTheme(getTheme());
}
