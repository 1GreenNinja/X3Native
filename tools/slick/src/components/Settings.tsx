// Settings.tsx — account settings. One job that matters: change your password,
// as simply as possible. Three fields, one button, clear feedback.

import { useState } from "preact/hooks";
import { type Session, changePassword } from "../client";

export function Settings({ session, onClose }: { session: Session; onClose: () => void }) {
  const [current, setCurrent] = useState("");
  const [next, setNext] = useState("");
  const [confirm, setConfirm] = useState("");
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState<{ ok: boolean; text: string } | null>(null);

  const save = async () => {
    setMsg(null);
    if (next.length < 6) return setMsg({ ok: false, text: "New password needs at least 6 characters." });
    if (next !== confirm) return setMsg({ ok: false, text: "The two new passwords don't match." });
    setBusy(true);
    try {
      await changePassword(session.token, session.userId, current, next);
      setMsg({ ok: true, text: "✓ Password changed. You're all set." });
      setCurrent(""); setNext(""); setConfirm("");
    } catch (e: any) {
      setMsg({ ok: false, text: e?.message ?? "Couldn't change password." });
    } finally {
      setBusy(false);
    }
  };

  const ready = current && next && confirm && !busy;

  return (
    <div class="genpanel-backdrop" onClick={onClose}>
      <div class="genpanel settings" onClick={(e) => e.stopPropagation()}>
        <header class="genpanel-head">
          <span>⚙️ Settings — {session.userId.split(":")[0]}</span>
          <button class="genpanel-x" onClick={onClose}>✕</button>
        </header>

        <h3 class="settings-h3">Change password</h3>
        <input class="genpanel-prompt" type="password" placeholder="Current password"
          value={current} autofocus
          onInput={(e) => setCurrent((e.target as HTMLInputElement).value)} />
        <input class="genpanel-prompt" type="password" placeholder="New password"
          value={next}
          onInput={(e) => setNext((e.target as HTMLInputElement).value)} />
        <input class="genpanel-prompt" type="password" placeholder="Confirm new password"
          value={confirm}
          onInput={(e) => setConfirm((e.target as HTMLInputElement).value)}
          onKeyDown={(e) => { if (e.key === "Enter" && ready) save(); }} />

        {msg && <div class={msg.ok ? "settings-ok" : "settings-err"}>{msg.text}</div>}

        <button class="genpanel-go" disabled={!ready} onClick={save}>
          {busy ? "Saving…" : "Change password"}
        </button>

        <div class="settings-note">
          Forgot your current password? You're the server admin — in Element's
          <code>@conduit</code> room send <code>@conduit: reset-password {session.userId.split(":")[0].replace("@","")}</code>
          to get a fresh one, then change it here.
        </div>
      </div>
    </div>
  );
}
