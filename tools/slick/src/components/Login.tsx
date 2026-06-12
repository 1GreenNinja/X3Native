import { useState } from "preact/hooks";
import { type Session, login, HOMESERVER } from "../client";

export function Login({ onLogin }: { onLogin: (s: Session) => void }) {
  const [username, setUsername] = useState("");
  const [password, setPassword] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");

  const submit = async (e: Event) => {
    e.preventDefault();
    setBusy(true);
    setError("");
    try {
      onLogin(await login(username.trim(), password));
    } catch (err: any) {
      setError(err?.message ?? "Login failed");
    } finally {
      setBusy(false);
    }
  };

  return (
    <div class="login-page">
      <form class="login-card" onSubmit={submit}>
        <h1>Slick</h1>
        <p class="login-sub">FleetCommand — {HOMESERVER.replace("https://", "")}</p>
        <input
          placeholder="username (e.g. tim)"
          value={username}
          onInput={(e) => setUsername((e.target as HTMLInputElement).value)}
          autofocus
        />
        <input
          type="password"
          placeholder="password"
          value={password}
          onInput={(e) => setPassword((e.target as HTMLInputElement).value)}
        />
        {error && <div class="login-error">{error}</div>}
        <button type="submit" disabled={busy || !username || !password}>
          {busy ? "Signing in…" : "Sign in"}
        </button>
      </form>
    </div>
  );
}
