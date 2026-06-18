// main.js — Slick desktop (Electron). Wraps the same built dist/ that the web
// app serves, so the Windows .exe and the browser are pixel-identical. This is
// the same architecture Slack/Discord/VSCode ship: a packaged web app with its
// own Chromium, no dependency on the system WebView.
//
// Two run modes:
//   - LIVE (default): loads https://slick.x3designs.net (or slopclaude) so the
//     desktop app tracks the deployed build automatically. Set SLICK_URL to
//     override.
//   - LOCAL: if SLICK_LOCAL=1, loads the bundled ../dist/index.html so the app
//     works with no tunnel (useful offline / on the LAN).

const { app, BrowserWindow, shell, Menu } = require("electron");
const path = require("path");

const LIVE_URL = process.env.SLICK_URL || "https://slick.slopclaude.com";
const USE_LOCAL = process.env.SLICK_LOCAL === "1";

function createWindow() {
  const win = new BrowserWindow({
    width: 1280,
    height: 820,
    minWidth: 720,
    minHeight: 480,
    backgroundColor: "#0a0e14", // matches Slick's gunmetal base — no white flash
    title: "Slick",
    autoHideMenuBar: true,
    webPreferences: {
      preload: path.join(__dirname, "preload.js"),
      contextIsolation: true,
      nodeIntegration: false,
    },
  });

  if (USE_LOCAL) {
    win.loadFile(path.join(__dirname, "..", "dist", "index.html"));
  } else {
    win.loadURL(LIVE_URL);
  }

  // External links (e.g. image "Download original") open in the real browser,
  // not inside the app window.
  win.webContents.setWindowOpenHandler(({ url }) => {
    if (!url.startsWith(LIVE_URL)) {
      shell.openExternal(url);
      return { action: "deny" };
    }
    return { action: "allow" };
  });

  // Keyboard accelerators (the menu is hidden, so wire these by hand):
  //   Ctrl+R / F5            → reload (pull the latest deployed build)
  //   Ctrl+Shift+R           → hard reload (ignore cache)
  //   F12 / Ctrl+Shift+I     → toggle devtools
  win.webContents.on("before-input-event", (event, input) => {
    if (input.type !== "keyDown") return;
    const ctrl = input.control || input.meta;
    const key = (input.key || "").toLowerCase();
    if ((ctrl && key === "r") || key === "f5") {
      if (ctrl && input.shift) win.webContents.reloadIgnoringCache();
      else win.webContents.reload();
      event.preventDefault();
    } else if (key === "f12" || (ctrl && input.shift && key === "i")) {
      win.webContents.toggleDevTools();
      event.preventDefault();
    }
  });
}

app.whenReady().then(() => {
  Menu.setApplicationMenu(null); // chrome-less, like Slack
  createWindow();
  app.on("activate", () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow();
  });
});

app.on("window-all-closed", () => {
  if (process.platform !== "darwin") app.quit();
});
