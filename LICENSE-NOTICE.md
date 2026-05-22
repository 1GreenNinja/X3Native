# License Notice

## X3Native is original work — no copyleft, no engine fork

X3Native (the `engine/`, `app/`, and `shaders/` code) was written **clean-room from scratch**: implemented from behavioral specs (`specs/`) + public technical references + the author's own research. **No id Tech / RBDOOM / Doom 3 — or any other third-party engine — source was forked, copied, or consulted.** See `PROVENANCE.md` for the originality record and `THIRDPARTY_LICENSES.md` for the (permissive-only) dependency list.

Copyright protects expression, not ideas. Studying public architecture material (Vulkan spec, *Real-Time Rendering*, GPU Gems, public GDC/SIGGRAPH talks) and writing your own implementation is independent creation, not a derivative work — and that is exactly how this engine was built.

## What you can do

- ✅ **Engine** (`X3Engine.exe`): Tim Smith's wholly-owned IP. May be shipped **closed-source and sold commercially**, with no copyleft obligation.
- ✅ **Game data** (`.x3pak` — meshes, textures, audio, Lua scripts): a separate work, commercial at any time.
- ✅ The repo may be **public or private** at the author's discretion — there is no GPL or other license forcing it open.

## Third-party libraries — permissive only

Every third-party dependency is under a permissive license (MIT / Apache 2.0 / zlib / BSD / public-domain) and is shippable in a closed-source commercial build. The authoritative list is `THIRDPARTY_LICENSES.md`.

| Library | License | Use |
|---|---|---|
| vk-bootstrap | MIT | Vulkan init |
| VulkanMemoryAllocator (VMA) | MIT | GPU memory |
| GLFW | zlib | window/input |
| Jolt Physics | MIT | physics |
| cgltf | MIT | glTF loading |
| stb (image / image_write) | MIT / public-domain | image I/O |
| miniaudio | MIT / public-domain | audio |
| miniz | MIT | pak/zip |
| glm | MIT | math |
| sol3 + Lua/LuaJIT | MIT | scripting (when added) |
| basis_universal | Apache 2.0 | KTX2 textures (when added) |
| Dear ImGui | MIT | dev UI (when added) |
| Tracy | BSD-3 | profiler (when added) |
| font8x8 | public-domain | bitmap HUD font |

**Do NOT add any GPL / LGPL / CC-BY-SA third-party code** — it would impose copyleft obligations on the engine and forfeit the clean, sellable IP position.
