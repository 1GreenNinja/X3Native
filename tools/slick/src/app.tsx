import { useEffect, useState } from "preact/hooks";
import { type Session, loadSession, clearSession } from "./client";
import { startSyncLoop, stopSyncLoop } from "./store";
import { Login } from "./components/Login";
import { Workspace } from "./components/Workspace";

export function App() {
  const [session, setSession] = useState<Session | null>(loadSession());

  useEffect(() => {
    if (session) {
      startSyncLoop(session);
      return () => stopSyncLoop();
    }
  }, [session]);

  if (!session) {
    return <Login onLogin={setSession} />;
  }

  const logout = () => {
    stopSyncLoop();
    clearSession();
    setSession(null);
  };

  return <Workspace session={session} onLogout={logout} />;
}
