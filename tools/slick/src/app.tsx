import { useEffect, useState } from "preact/hooks";
import { type Session, loadSession, clearSession } from "./client";
import { startSyncLoop, stopSyncLoop } from "./store";
import { getTheme } from "./theme";
import { Login } from "./components/Login";
import { Workspace } from "./components/Workspace";

function BootSplash() {
  return (
    <div class="boot">
      <div class="boot-inner">
        <div class="boot-logo">SLICK</div>
        <div class="boot-line">▸ CONNECTING TO FLEETCOMMAND…</div>
        <div class="boot-bar"><i /></div>
      </div>
    </div>
  );
}

export function App() {
  const [session, setSession] = useState<Session | null>(loadSession());
  const [booting, setBooting] = useState(getTheme().splash);
  useEffect(() => {
    if (!booting) return;
    const t = setTimeout(() => setBooting(false), 1600);
    return () => clearTimeout(t);
  }, []);

  useEffect(() => {
    if (session) {
      startSyncLoop(session);
      return () => stopSyncLoop();
    }
  }, [session]);

  const logout = () => {
    stopSyncLoop();
    clearSession();
    setSession(null);
  };

  return (
    <>
      {booting && <BootSplash />}
      {session ? <Workspace session={session} onLogout={logout} /> : <Login onLogin={setSession} />}
    </>
  );
}
