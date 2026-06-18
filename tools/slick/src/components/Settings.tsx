// Settings.tsx — tabbed control panel: Account / Appearance / Admin.
// ctOS-flavored. Admin tab only appears for the server admin.

import { useEffect, useState } from "preact/hooks";
import { type Session, changePassword, adminCommand, isServerAdmin } from "../client";
import { getTheme, setTheme, THEME_PRESETS, type Theme } from "../theme";

type Tab = "account" | "appearance" | "admin";

export function Settings({ session, onClose }: { session: Session; onClose: () => void }) {
  const [tab, setTab] = useState<Tab>("account");
  const [admin, setAdmin] = useState(false);

  useEffect(() => {
    isServerAdmin(session.token, session.userId).then(setAdmin);
  }, []);

  return (
    <div class="genpanel-backdrop" onClick={onClose}>
      <div class="genpanel settings wide" onClick={(e) => e.stopPropagation()}>
        <header class="genpanel-head">
          <span>⚙ SETTINGS — {session.userId.split(":")[0]}</span>
          <button class="genpanel-x" onClick={onClose}>✕</button>
        </header>

        <div class="set-tabs">
          <button class={tab === "account" ? "on" : ""} onClick={() => setTab("account")}>Account</button>
          <button class={tab === "appearance" ? "on" : ""} onClick={() => setTab("appearance")}>Appearance</button>
          {admin && <button class={tab === "admin" ? "on" : ""} onClick={() => setTab("admin")}>Admin</button>}
        </div>

        <div class="set-body">
          {tab === "account" && <AccountTab session={session} />}
          {tab === "appearance" && <AppearanceTab />}
          {tab === "admin" && admin && <AdminTab session={session} />}
        </div>
      </div>
    </div>
  );
}

// ---------------------------------------------------------------- Account
function AccountTab({ session }: { session: Session }) {
  const [current, setCurrent] = useState("");
  const [next, setNext] = useState("");
  const [confirm, setConfirm] = useState("");
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState<{ ok: boolean; text: string } | null>(null);

  const save = async () => {
    setMsg(null);
    if (next.length < 6) return setMsg({ ok: false, text: "New password needs 6+ characters." });
    if (next !== confirm) return setMsg({ ok: false, text: "New passwords don't match." });
    setBusy(true);
    try {
      await changePassword(session.token, session.userId, current, next);
      setMsg({ ok: true, text: "✓ Password changed." });
      setCurrent(""); setNext(""); setConfirm("");
    } catch (e: any) {
      setMsg({ ok: false, text: e?.message ?? "Couldn't change password." });
    } finally { setBusy(false); }
  };
  const ready = current && next && confirm && !busy;

  return (
    <>
      <div class="settings-account">
        <div class="settings-field"><label>Account</label><span>{session.userId}</span></div>
        <div class="settings-field"><label>Device</label><span>{session.deviceId}</span></div>
      </div>
      <h3 class="settings-h3">Change password</h3>
      <input class="genpanel-prompt" type="password" placeholder="Current password" value={current}
        onInput={(e) => setCurrent((e.target as HTMLInputElement).value)} />
      <input class="genpanel-prompt" type="password" placeholder="New password" value={next}
        onInput={(e) => setNext((e.target as HTMLInputElement).value)} />
      <input class="genpanel-prompt" type="password" placeholder="Confirm new password" value={confirm}
        onInput={(e) => setConfirm((e.target as HTMLInputElement).value)}
        onKeyDown={(e) => { if (e.key === "Enter" && ready) save(); }} />
      {msg && <div class={msg.ok ? "settings-ok" : "settings-err"}>{msg.text}</div>}
      <button class="genpanel-go" disabled={!ready} onClick={save}>{busy ? "Saving…" : "Change password"}</button>
    </>
  );
}

// ------------------------------------------------------------- Appearance
function AppearanceTab() {
  const [t, setT] = useState<Theme>(getTheme());
  const apply = (next: Theme) => { setT(next); setTheme(next); };

  return (
    <>
      <h3 class="settings-h3">Theme</h3>
      <div class="theme-presets">
        {Object.entries(THEME_PRESETS).map(([name, preset]) => (
          <button key={name} class="theme-chip" style={{ borderColor: preset.accent }}
            onClick={() => apply({ ...preset })}>
            <span class="theme-swatch" style={{ background: preset.accent }} />
            {name}
          </button>
        ))}
      </div>

      <div class="genpanel-row">
        <label>Accent</label>
        <input type="color" value={t.accent}
          onInput={(e) => apply({ ...t, accent: (e.target as HTMLInputElement).value })} />
        <span class="set-val">{t.accent}</span>
      </div>

      <div class="genpanel-row">
        <label>Glass</label>
        <input type="range" min={0} max={100} value={Math.round(t.glass * 100)}
          onInput={(e) => apply({ ...t, glass: Number((e.target as HTMLInputElement).value) / 100 })} />
        <span class="set-val">{Math.round(t.glass * 100)}%</span>
      </div>

      <h3 class="settings-h3">Effects</h3>
      {([
        ["grid", "Plating grid"],
        ["scanlines", "CRT scanlines"],
        ["glow", "Hover glow"],
        ["motion", "Message motion"],
        ["splash", "Boot splash"],
      ] as [keyof Theme, string][]).map(([key, label]) => (
        <div class="genpanel-row" key={key}>
          <label>{label}</label>
          <button class={`toggle ${t[key] ? "on" : ""}`}
            onClick={() => apply({ ...t, [key]: !t[key] })}>
            {t[key] ? "On" : "Off"}
          </button>
        </div>
      ))}
      <div class="settings-note">Lower glass = more see-through. All effects apply live and persist;
        boot splash shows on next launch.</div>
    </>
  );
}

// ------------------------------------------------------------------ Admin
function AdminTab({ session }: { session: Session }) {
  const [newUser, setNewUser] = useState("");
  const [newPass, setNewPass] = useState("");
  const [target, setTarget] = useState("");
  const [out, setOut] = useState("");
  const [busy, setBusy] = useState(false);

  const run = async (command: string) => {
    setBusy(true); setOut("Running…");
    try {
      setOut(await adminCommand(session.token, session.userId, command));
    } catch (e: any) {
      setOut(e?.message ?? "Command failed.");
    } finally { setBusy(false); }
  };

  return (
    <>
      <h3 class="settings-h3">Create user</h3>
      <div class="admin-row">
        <input class="genpanel-prompt" placeholder="username" value={newUser}
          onInput={(e) => setNewUser((e.target as HTMLInputElement).value)} />
        <input class="genpanel-prompt" placeholder="password (blank = generate)" value={newPass}
          onInput={(e) => setNewPass((e.target as HTMLInputElement).value)} />
        <button class="admin-btn" disabled={busy || !newUser}
          onClick={() => run(`create-user ${newUser}${newPass ? " " + newPass : ""}`)}>Create</button>
      </div>

      <h3 class="settings-h3">Reset password</h3>
      <div class="admin-row">
        <input class="genpanel-prompt" placeholder="username" value={target}
          onInput={(e) => setTarget((e.target as HTMLInputElement).value)} />
        <button class="admin-btn" disabled={busy || !target}
          onClick={() => run(`reset-password ${target}`)}>Reset (generates)</button>
      </div>

      <h3 class="settings-h3">Deactivate user</h3>
      <div class="admin-row">
        <button class="admin-btn danger" disabled={busy || !target}
          onClick={() => run(`deactivate-user ${target}`)}>Deactivate “{target || "username above"}”</button>
      </div>

      {out && <pre class="admin-out">{out}</pre>}
      <div class="settings-note">Commands run through the @conduit admin bot. reset-password & create-user
        return a generated password if you don't supply one — copy it from the output.</div>
    </>
  );
}
