# Conduit deployment on 13700K — Windows reality

The Phase 1 plan originally assumed a pre-built `conduit-x86_64-pc-windows-msvc.exe` would be on Conduit's GitLab releases. **It isn't.** Conduit only publishes Linux/macOS binaries + source. Three viable paths for the Windows host (13700K):

## Option 1 — Docker Desktop on 13700K (recommended)

Cleanest. Conduit publishes the official image `matrixconduit/matrix-conduit:v0.10.12` on Docker Hub. Setup is ~10 min once Docker Desktop is installed.

**Pros:** mature image, automatic restart, isolated from the host, easy upgrade path.
**Cons:** Docker Desktop is ~5 GB install + uses a few GB RAM (manageable on 13700K).

**Setup outline (run on 13700K):**

```powershell
# 1. Install Docker Desktop
winget install Docker.DockerDesktop --silent --accept-package-agreements

# 2. Reboot if prompted (Docker Desktop wants WSL2 enabled)

# 3. Create the data volume and config directory
New-Item -ItemType Directory -Force -Path 'C:\opt\conduit\data'
New-Item -ItemType Directory -Force -Path 'C:\opt\conduit\config'

# 4. Write the conduit.toml config (same as in the plan Task 6)
# (Generate the registration token and substitute it before writing)

# 5. Run the container with restart=always so it survives reboots
docker run -d --name conduit `
  --restart unless-stopped `
  -p 127.0.0.1:6167:6167 `
  -v C:\opt\conduit\data:/var/lib/matrix-conduit `
  -v C:\opt\conduit\config\conduit.toml:/etc/matrix-conduit/conduit.toml:ro `
  -e CONDUIT_CONFIG=/etc/matrix-conduit/conduit.toml `
  matrixconduit/matrix-conduit:v0.10.12

# 6. Verify
Invoke-RestMethod -Uri 'http://127.0.0.1:6167/_matrix/client/versions'
```

## Option 2 — WSL2 + Linux binary

13700K likely has WSL2 already (Tim mentioned the I4400 install). The Linux binary download IS published on GitLab.

**Setup outline:**

```bash
# Inside WSL2 Ubuntu shell on 13700K
sudo apt update
sudo apt install -y wget
mkdir -p ~/conduit && cd ~/conduit
wget https://gitlab.com/famedly/conduit/-/jobs/artifacts/v0.10.12/raw/conduit-x86_64-unknown-linux-musl?job=docker:build -O conduit
chmod +x conduit
# Note: URL may need adjustment - check gitlab CI artifacts for the actual current path
# Alternative: use the deb package from packages.gitlab.com
```

**Pros:** native Linux binary, no Docker layer, lighter resource footprint.
**Cons:** WSL2 networking quirks (need to bridge port 6167 from WSL → Windows for cloudflared on the host to reach it), or run cloudflared inside WSL too.

## Option 3 — Build from source on Windows with Rust

Cargo can build Conduit natively for Windows.

**Setup outline:**

```powershell
# 1. Install Rust
winget install Rustlang.Rustup --silent --accept-package-agreements
# (or download rustup-init.exe from https://win.rustup.rs/x86_64)

# 2. After install, open a NEW PowerShell so PATH picks up cargo
rustup default stable

# 3. Build Conduit from the source tarball at this location
cd D:\GameDev\X3Native\tools\conduit-prep
tar -xzf conduit-v0.10.12.tar.gz
cd conduit-v0.10.12

# 4. Build release binary (~10 min, ~2 GB toolchain footprint)
cargo build --release --bin conduit --features sqlite

# 5. Binary lands at: target\release\conduit.exe
Copy-Item target\release\conduit.exe C:\opt\conduit\conduit.exe
```

**Pros:** native exe, no Docker / WSL overhead, easiest to operationalize as a Windows Scheduled Task.
**Cons:** ~2 GB Rust toolchain install, 10-min first build, you become the maintainer of the build process.

## My recommendation

**Option 1 (Docker)** is the cleanest. The Phase 1 plan was written assuming a Windows-native binary; updating Task 5 of the plan to use Docker is a 5-minute change.

**Option 3 (build from source)** is the most "no abstractions" choice and matches the spec's "single Rust binary" vibe. Worth doing if Tim wants Conduit as a Windows Scheduled Task with no Docker Desktop dependency.

## Files in this staging directory

| File | Purpose |
|---|---|
| `conduit-v0.10.12.tar.gz` | Conduit v0.10.12 source archive — already downloaded |
| `CONDUIT-DEPLOY.md` | This document |

## SHA256 of the source archive

```
51ace710d645677c71d7ef99bbdfdd5dd78d179d1180bca5b3cd54426c11ddd4  conduit-v0.10.12.tar.gz
```

## What gets updated in the Phase 1 plan on Tim's wake

Plan Task 5 ("Download the Conduit binary") needs to be split into:
- 5a: Pick deployment option (1, 2, or 3)
- 5b: Execute that option's setup steps

And Plan Task 8 ("Install Conduit as a Windows Scheduled Task") only applies to Option 3 (native exe). For Option 1 (Docker), the equivalent is `--restart unless-stopped` on the container. For Option 2 (WSL2), it's a systemd service inside WSL.
