// Settings.tsx — account settings (PR-8). Primary feature: easy password
// change. Matrix requires the CURRENT password to re-authenticate; for the
// "I never knew it" case, the admin-reset note points at the @conduit room.

import { useState } from "preact/hooks";
import { type Session, changePassword } from "../client";

export function Settings({ session, onClose }: { session: Session; onClose: () => void }) {
  const [current, setCurrent] = useState("");
  const [next, setNext] = useState("");
  const [confirm, setConfirm] = useState("");
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState<{ kind: "ok" | "err"; text: string } | null>(null);

  const save = async () => {
    setMsg(null);
    if (next.length < 6) { setMsg({ kind: "err", text: "New password must be at least 6 characters." }); return; }
    if (next !== confirm) { setMsg({ kind: "err", text: "New passwords don't match." }); return; }
    setBusy(true);
    try {
      await changePassword(session.token, session.userId, current, next);
      setMsg({ kind: "ok", text: "Password changed. Other sessions stay signed in." });
      setCurrent(""); setNext(""); setConfirm("");
    } catch (e: any) {
      setMsg({ kind: "err", text: e?.message ?? "Change failed — check your current password." });
    } finally {
      setBusy(false);
    }
  };

  return (
    <div class="genpanel-backdrop" onClick={onClose}>
      <div class="genpanel settings" onClick={(e) => e.stopPropagation()}>
        <header class="genpanel-head">
          <span>⚙️ Settings</span>
          <button class="genpanel-x" onClick={onClose}>✕</button>
        </header>

        <div class="settings-account">
          <div class="settings-field"><label>Account</label><span>{session.userId}</span></div>
          <div class="settings-field"><label>Device</label><span>{session.deviceId}</span></div>
        </div>

        <h3 class="settings-h3">Change password</h3>
        <input class="genpanel-prompt" type="password" placeholder="current password"
          value={current} onInput={(e) => setCurrent((e.target as HTMLInputElement).value)} />
        <input class="genpanel-prompt" type="password" placeholder="new password"
          value={next} onInput={(e) => setNext((e.target as HTMLInputElement).value)} />
        <input class="genpanel-prompt" type="password" placeholder="confirm new password"
          value={confirm} onInput={(e) => setConfirm((e.target as HTMLInputElement).value)} />

        {msg && <div class={msg.kind === "ok" ? "settings-ok" : "settings-err"}>{msg.text}</div>}

        <button class="genpanel-go" disabled={busy || !current || !next || !confirm} onClick={save}>
          {busy ? "Saving…" : "Update password"}
        </button>

        <div class="settings-note">
          Don't know your current password? If you're the server admin, reset it
          from the <code>@conduit</code> room in Element with no old password needed:
          <br />
          <code>reset-password {session.userId} &lt;new&gt;</code>
        </div>
      </div>
    </div>
  );
}
