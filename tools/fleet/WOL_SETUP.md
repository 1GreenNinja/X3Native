# Wake-on-LAN — fleet power-saving setup

Goal: forge/render boxes **sleep to save power** and **wake on demand** for gen
jobs. The always-on box (13700K: homeserver + Integrator) sends the wake signal
via `tools/fleet/wol.py`. Cuts the electric bill without losing the fleet.

## Send a wake (from the 13700K, always-on)
```powershell
python G:\X3Native\tools\fleet\wol.py 14900k     # wake by name
python G:\X3Native\tools\fleet\wol.py all        # wake everyone
python G:\X3Native\tools\fleet\wol.py --list     # show the registry
```
The MAC for each box lives in `fleet_boxes.json`. **Add yours** (below) so it's
wakeable.

## Per-box enablement — BOTH gates must be ON (this is the part Tim flagged)

WoL waking a box from sleep (S3) or full-off (S5) needs THREE things set, per box:

### 1. BIOS / UEFI  ⟵ the one people forget
Reboot into BIOS. Find and ENABLE (name varies by board):
- **"Wake on LAN"** / **"Power On By PCI-E / PCIe"** / **"Resume By PCI-E Device"**
- On some boards also: **"ErP Ready" = Disabled** (ErP/deep-off can kill WoL from S5)
Save + exit. Without this, no magic packet will ever wake the box.

### 2. Windows NIC driver
```powershell
# check current state
Get-NetAdapterPowerManagement -Name Ethernet | Select WakeOnMagicPacket, WakeOnPattern
# enable magic-packet wake
Enable-NetAdapterPowerManagement -Name Ethernet
Set-NetAdapterPowerManagement -Name Ethernet -WakeOnMagicPacket Enabled
```
Also in Device Manager → your NIC → Power Management: check **"Allow this device
to wake the computer"** and **"Only allow a magic packet…"**. Disable **"Energy
Efficient Ethernet"** / **"Green Ethernet"** if wake is flaky.

### 3. Disable Fast Startup (breaks WoL from full shutdown on many boards)
```powershell
powercfg /hibernate off
# or: Control Panel > Power Options > "Choose what the power buttons do"
#     > uncheck "Turn on fast startup"
```
Fast Startup puts the NIC in a state that ignores magic packets after a full
shutdown. Sleep (S3) usually still works with it on, but off (S5) often won't.

## Register your box for waking
Run this and report the Physical Address to the fleet (or add it yourself):
```powershell
getmac /v /fo list          # find the Ethernet "Physical Address"
ipconfig | findstr IPv4     # and your LAN IP
```
Then set your box's `mac` + `ip` in `tools/fleet/fleet_boxes.json`.

## Verify
From the 13700K: `python wol.py <yourbox>` while your box is asleep — it should
POST within ~15–60s. If nothing: re-check BIOS (#1) and Fast Startup (#3) first.

## Next (optional automation)
Once MACs are registered, `wol.py` can be wired into the `/gen` queue: when a job
targets a sleeping forge box, wake it, wait for it to come up, dispatch, and let
it sleep again after an idle timeout. Ask Fable to wire it when you want it.
