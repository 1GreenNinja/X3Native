# License Notice — READ BEFORE DISTRIBUTING

X3Native is in the **HYBRID** phase (see `X3_NATIVE_ENGINE_PLAN.md` §5).

## Current state: contains GPL-v3 code

While `engine/_gpl_rbdoom/` exists (or any RBDOOM-derived code remains per `GPL_DEBT.md`), the engine as a whole is a derivative work of **Doom 3 BFG / RBDOOM-3-BFG (GPL v3)**. Therefore:

- ✅ You may use, modify, and redistribute the engine **under GPL v3** (source must be available to anyone you give binaries to).
- ❌ You may **not** sell closed-source builds of the engine in this state.
- ✅ Game *data* (`.x3pak` — meshes, textures, audio, Lua) is a separate work and is **not** GPL-bound. It can be commercial at any time (the Quake 3 / Doom 3 model).

## Target state: clean-room complete

When `GPL_DEBT.md` is empty, `engine/_gpl_rbdoom/` is deleted, and the build is green with `USE_GPL_SCAFFOLD=OFF`:

- The engine is **100% original work** + permissively-licensed third-party libraries (MIT/Apache/zlib/BSD).
- It may then be **closed-source and sold commercially** with no GPL obligation.

## Third-party libraries (all permissive — track in THIRDPARTY_LICENSES.md as added)

| Library | License | Use |
|---|---|---|
| vk-bootstrap | MIT | Vulkan init |
| VMA | MIT | GPU memory |
| Jolt Physics | MIT | physics |
| cgltf | MIT | glTF loading |
| basis_universal | Apache 2.0 | KTX2 textures |
| miniaudio | MIT/public-domain | audio |
| sol3 + Lua/LuaJIT | MIT | scripting |
| Dear ImGui | MIT | dev UI |
| EnTT / flecs | MIT | ECS |
| glm | MIT | math |
| Tracy | BSD-3 | profiler |
| Recast/Detour | zlib | navmesh |
| miniz | MIT | pak/zip |

**Do NOT add any GPL/LGPL/CC-BY-SA third-party code** beyond the temporary RBDOOM scaffold — it would re-contaminate the clean target.
