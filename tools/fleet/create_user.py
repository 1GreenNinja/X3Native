#!/usr/bin/env python3
"""
create_user.py — CLI wrapper for Conduit's token-gated user registration.

Why this exists
---------------
Onboarding a new fleet member via the Element admin room is "just" a few
clicks but it's not scriptable. This wraps the same UIAA registration flow
in a one-shot CLI so onboarding is `python create_user.py <name> <pass>` and
you immediately get back the access token to drop on the new box.

The registration_token in C:\\opt\\conduit\\conduit.toml is the gate Tim chose
to keep (over full-open public registration). This script READS that token
locally, applies it to the registration request, and returns the credentials.

Usage
-----
    python tools/fleet/create_user.py rtsfable somesecurepassword
    python tools/fleet/create_user.py rtsfable somesecurepass --save-token G:\\rtsfable.matrix_token
    python tools/fleet/create_user.py rtsfable somesecurepass --quiet  # only print token

The created user is named @<username>:fleetcommand.slopclaude.com. The
script verifies the homeserver responds (won't write a token if Conduit's
down), prints the user_id + access_token + device_id, and optionally
chmods the saved token file to 600 (POSIX) / current-user-only (Windows).
"""

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

HOMESERVER = "https://fleetcommand.slopclaude.com"
CONDUIT_CONFIG = Path(r"C:\opt\conduit\conduit.toml")
USER_AGENT = "fleet-bot/1.0 (create_user.py)"


def read_registration_token(config_path: Path) -> str:
    """Pull registration_token out of conduit.toml via regex (no toml dep)."""
    if not config_path.exists():
        sys.exit(f"FATAL: Conduit config not found at {config_path}")
    txt = config_path.read_text(encoding="utf-8")
    match = re.search(r'^\s*registration_token\s*=\s*"([^"]+)"', txt, re.MULTILINE)
    if not match:
        sys.exit(
            f"FATAL: registration_token not found (or commented out) in {config_path}.\n"
            "Open registration is disabled — re-enable the token line or use a different path."
        )
    return match.group(1)


def http_post(url: str, body: dict, timeout: int = 10) -> dict:
    """POST JSON, return parsed JSON. Raises on non-2xx with clean error text."""
    data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(
        url, data=data,
        headers={"Content-Type": "application/json", "User-Agent": USER_AGENT},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        err_body = e.read().decode("utf-8", errors="replace")
        try:
            err_json = json.loads(err_body)
            errcode = err_json.get("errcode", "M_UNKNOWN")
            errmsg = err_json.get("error", err_body)
            sys.exit(f"FATAL: homeserver rejected ({e.code}): {errcode}: {errmsg}")
        except json.JSONDecodeError:
            sys.exit(f"FATAL: homeserver rejected ({e.code}): {err_body[:300]}")
    except urllib.error.URLError as e:
        sys.exit(f"FATAL: cannot reach {url}: {e.reason}")


def register(username: str, password: str, token: str) -> dict:
    """Two-step UIAA: probe for session, then submit auth + identity."""
    url = f"{HOMESERVER}/_matrix/client/v3/register"
    # Step 1: probe (empty body) to get a UIAA session ID
    probe = http_post(url, {})
    session = probe.get("session")
    if not session:
        sys.exit(f"FATAL: registration probe returned no session: {probe}")
    # Step 2: register with the token + identity
    payload = {
        "auth": {
            "type": "m.login.registration_token",
            "token": token,
            "session": session,
        },
        "username": username,
        "password": password,
        "inhibit_login": False,  # we want the access_token back
    }
    return http_post(url, payload)


def write_token_file(path: Path, token: str) -> None:
    """Atomic write + restrict perms."""
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(token, encoding="utf-8")
    os.replace(tmp, path)
    if os.name == "posix":
        os.chmod(path, 0o600)
    # On Windows, file inherits user's perms; that's adequate for ~/.claude/.matrix_token
    # pattern. If stricter ACLs are wanted, run icacls separately.


def run_gui() -> None:
    """Tkinter form for one-click user creation. Username + password fields, Submit button,
    result panel with credentials + a 'Copy access_token' button."""
    import tkinter as tk
    from tkinter import messagebox, ttk

    def on_submit():
        u = user_var.get().strip()
        p = pass_var.get()
        if not u or not p:
            messagebox.showerror("Missing field", "Both username and password are required.")
            return
        if not re.fullmatch(r"[a-z0-9._=\-]+", u):
            messagebox.showerror("Bad username", "Lowercase alphanumeric only (+ . _ = -)")
            return
        out_box.config(state="normal")
        out_box.delete("1.0", "end")
        out_box.insert("end", f"Registering @{u}:fleetcommand.slopclaude.com ...\n")
        out_box.update()
        try:
            token = read_registration_token(CONDUIT_CONFIG)
            result = register(u, p, token)
        except SystemExit as e:
            out_box.insert("end", f"\nFAILED: {e}\n")
            out_box.config(state="disabled")
            return
        uid = result.get("user_id"); at = result.get("access_token"); did = result.get("device_id")
        out_box.insert("end", f"\n  user_id:      {uid}\n  device_id:    {did}\n\n  access_token: {at}\n")
        out_box.insert("end", "\nNext: drop access_token at ~/.claude/.matrix_token on the new box,\n")
        out_box.insert("end", "run ~/.claude/matrix-daemon/daemon.js, install as Scheduled Task.\n")
        out_box.config(state="disabled")
        # cache for the copy button
        token_holder["t"] = at

    def on_copy():
        t = token_holder.get("t")
        if not t:
            messagebox.showinfo("Nothing to copy", "Register a user first.")
            return
        root.clipboard_clear(); root.clipboard_append(t); root.update()
        messagebox.showinfo("Copied", "access_token on clipboard.")

    root = tk.Tk()
    root.title("FleetCommand — Create User")
    root.geometry("560x420")
    token_holder: dict = {}
    frame = ttk.Frame(root, padding=12); frame.pack(fill="both", expand=True)
    ttk.Label(frame, text="Username (lowercase, no @ or :server)").grid(row=0, column=0, sticky="w")
    user_var = tk.StringVar()
    ttk.Entry(frame, textvariable=user_var, width=40).grid(row=1, column=0, columnspan=2, sticky="we", pady=(0, 8))
    ttk.Label(frame, text="Password").grid(row=2, column=0, sticky="w")
    pass_var = tk.StringVar()
    ttk.Entry(frame, textvariable=pass_var, show="*", width=40).grid(row=3, column=0, columnspan=2, sticky="we", pady=(0, 8))
    ttk.Button(frame, text="Create user", command=on_submit).grid(row=4, column=0, sticky="w", pady=(4, 8))
    ttk.Button(frame, text="Copy access_token", command=on_copy).grid(row=4, column=1, sticky="e", pady=(4, 8))
    out_box = tk.Text(frame, height=14, width=64, state="disabled", wrap="word")
    out_box.grid(row=5, column=0, columnspan=2, sticky="nsew")
    frame.rowconfigure(5, weight=1); frame.columnconfigure(0, weight=1)
    root.mainloop()


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Create a Conduit user via the registration_token UIAA flow.",
        epilog="Example: python create_user.py rtsfable s3cr3t --save-token G:\\rtsfable.matrix_token  |  python create_user.py --gui",
    )
    ap.add_argument("username", nargs="?", help="Localpart of the new user (no @ or :server)")
    ap.add_argument("password", nargs="?", help="Strong password for the new user")
    ap.add_argument("--gui", action="store_true",
                    help="Open the Tkinter GUI instead of a CLI invocation (no other args needed)")
    ap.add_argument("--save-token", type=Path, help="Write the access_token to this path")
    ap.add_argument("--token", default=None,
                    help="Override the registration_token (default: read from conduit.toml)")
    ap.add_argument("--quiet", action="store_true",
                    help="Print only the access_token to stdout (for piping)")
    args = ap.parse_args()

    if args.gui:
        run_gui()
        return

    if not args.username or not args.password:
        ap.error("username and password are required (or use --gui)")

    if not re.fullmatch(r"[a-z0-9._=\-]+", args.username):
        sys.exit("FATAL: username must be lowercase alphanumeric (+ . _ = -); no @ or :server.")

    token = args.token or read_registration_token(CONDUIT_CONFIG)
    result = register(args.username, args.password, token)

    user_id = result.get("user_id")
    access_token = result.get("access_token")
    device_id = result.get("device_id")

    if not access_token:
        sys.exit(f"FATAL: registration succeeded but no access_token returned: {result}")

    if args.save_token:
        write_token_file(args.save_token, access_token)

    if args.quiet:
        print(access_token)
        return

    print("=" * 60)
    print(f"  user_id:      {user_id}")
    print(f"  access_token: {access_token}")
    print(f"  device_id:    {device_id}")
    print("=" * 60)
    if args.save_token:
        print(f"  token written to: {args.save_token}")
    print()
    print("Next steps for the new fleet member:")
    print(f"  1. Drop the access_token at ~/.claude/.matrix_token on their box")
    print(f"  2. Copy ~/.claude/matrix-daemon/ from any existing box")
    print(f"  3. Run: MATRIX_BOT_MACHINE=<their-box> node ~/.claude/matrix-daemon/daemon.js")
    print(f"  4. Install as Scheduled Task <machine>-MatrixDaemon for boot persistence")
    print(f"  5. Invite {user_id} to Fleet Ops from your @tim Element session")
    print()


if __name__ == "__main__":
    main()
