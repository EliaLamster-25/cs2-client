# crymore.pw — Development vs Production

## Development workflow (daily work)

Use **Debug** builds and optional env vars. Security gates are relaxed so you can iterate quickly.

```powershell
# 1. Configure (once, or after CMake changes)
cmake -B build -DBUILD_CRYMORE_LOADER=ON

# 2. Build overlay (Debug) + loader
cmake --build build --config Debug --target cs2_overlay crymore_core crymore_loader_wpf

# 3. Run loader
$env:CRYMORE_DEV = "1"   # optional: dev auth bypass (any user/pass)
.\build\loader\Debug\Crymore.Loader.exe
```

**What Debug enables automatically:**

| Feature | Debug | Release |
|---------|-------|---------|
| `LOADER_DEV_LAUNCH` | sibling overlay first | embedded payload only |
| `demo/demo` login | yes (without API) | no |
| `CRYMORE_DEV` env bypass | yes | ignored |
| Overlay standalone run | yes with `CRYMORE_DEV=1` | blocked |
| Launch handshake | still used when via loader | required |

**Overlay-only dev** (without loader):

```powershell
$env:CRYMORE_DEV = "1"
.\build\Debug\<random_overlay_name>.exe
```

Only works on **Debug** overlay builds.

---

## Production / ship workflow

Use **Release** builds. No dev bypasses, no demo login, overlay must be started by the loader.

```powershell
# Full ship build
.\loader\tools\build_ship.ps1

# Output folder:
#   build\loader\Release\
#     Crymore.Loader.exe
#     crymore_core.dll
#     Assets\logo.png
#     Assets\app.ico
#   (payload embedded inside crymore_core.dll)
```

**What Release enforces:**

- `CRYMORE_RELEASE=1` on loader — no demo login, no `CRYMORE_DEV` bypass
- `CRYMORE_REQUIRE_LAUNCH_GATE=1` on overlay — exits if not launched by loader
- HMAC launch handshake via inherited pipe (90s TTL, parent PID check)
- Session re-validated before inject
- Periodic debugger/tool scan every 5s while logged in
- No sibling-overlay fallback

**Distribute only:**

```
Crymore.Loader.exe
crymore_core.dll
Assets\
```

Do **not** ship the raw overlay `.exe` separately — it is inside `payload.bin` embedded in `crymore_core.dll`.

---

## Before going live with auth API

1. Implement JSON parsing in `auth.cpp` for `POST /v1/auth/login`
2. Add TLS cert pinning in `httpPostJson`
3. Remove or gate any remaining offline paths
4. Rotate `kBuildPepper` in `launch_handshake.cpp` per release (optional)
5. Reconfigure cmake to rotate overlay/driver names: `cmake -B build`

---

## Security layers (current)

1. **Login required** — loader blocks inject without valid session
2. **Launch handshake** — overlay verifies HMAC + parent loader PID
3. **Anti-debug / anti-tool** — hard block at boot + periodic while logged in
4. **Environment checks** — blocks competing anti-cheats
5. **Embedded payload** — overlay not shipped as standalone exe
6. **DPAPI** — remembered credentials encrypted locally
7. **HWID binding** — ready for server-side enforcement

## Recommended next steps for distribution

- Wire live auth API + JWT validation
- Encrypt payload archive (AES-256, key from server ticket)
- Commercial packer on `crymore_core.dll` + embedded overlay
- Code signing certificate for loader exe
