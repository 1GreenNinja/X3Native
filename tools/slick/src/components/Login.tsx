import { useState } from "preact/hooks";
import { type Session, login, loginWithToken, HOMESERVER } from "../client";

export function Login({ onLogin }: { onLogin: (s: Session) => void }) {
  const [mode, setMode] = useState<"password" | "token">("password");
  const [username, setUsername] = useState("");
  const [password, setPassword] = useState("");
  const [token, setToken] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");

  const submit = async (e: Event) => {
    e.preventDefault();
    setBusy(true);
    setError("");
    try {
      if (mode === "password") {
        onLogin(await login(username.trim(), password));
      } else {
        onLogin(await loginWithToken(token));
      }
    } catch (err: any) {
      setError(err?.message ?? "Sign-in failed");
    } finally {
      setBusy(false);
    }
  };

  const canSubmit = mode === "password" ? username && password : token.trim();

  return (
    <div class="login-page">
      <form class="login-card" onSubmit={submit}>
        <h1>Slick</h1>
        <p class="login-tagline">Tired of the slog? Give it the slip.</p>
        <p class="login-sub">FleetCommand — {HOMESERVER.replace("https://", "")}</p>

        <div class="login-tabs">
          <button type="button" class={mode === "password" ? "on" : ""} onClick={() => setMode("password")}>Password</button>
          <button type="button" class={mode === "token" ? "on" : ""} onClick={() => setMode("token")}>Access token</button>
        </div>

        {mode === "password" ? (
          <>
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
          </>
        ) : (
          <>
            <input
              placeholder="paste access token (syt_…)"
              value={token}
              onInput={(e) => setToken((e.target as HTMLInputElement).value)}
              autofocus
            />
            <p class="login-hint">Element → Settings → Help &amp; About → Access Token. No password needed.</p>
          </>
        )}

        {error && <div class="login-error">{error}</div>}
        <button type="submit" disabled={busy || !canSubmit}>
          {busy ? "Signing in…" : "Sign in"}
        </button>
      </form>
    </div>
  );
}
