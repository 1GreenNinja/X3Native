// preload.js — runs before the page loads with contextIsolation on. Slick is a
// plain web app that talks to Conduit over HTTPS, so it needs nothing from the
// Electron main process today. This stub exists so the security model is
// explicit (no nodeIntegration, isolated context) and there's a seam to add
// native features later (notifications, tray, deep links) without reopening
// the sandbox.

window.addEventListener("DOMContentLoaded", () => {
  // intentionally empty — reserved for future native bridges
});
