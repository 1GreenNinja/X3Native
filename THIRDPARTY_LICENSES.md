# Third-Party Licenses

X3Native's own code is original and proprietary (see `LICENSE` and `PROVENANCE.md`).
The only third-party code in the project is the libraries below — **all permissive,
all shippable in a closed-source commercial build.** There are **no GPL / LGPL /
CC-BY-SA dependencies**, and none are permitted (adding one would impose copyleft
obligations on the engine).

## Currently linked (see `vcpkg.json`)

| Library | License | Use | Source |
|---|---|---|---|
| vk-bootstrap | MIT | Vulkan instance/device/swapchain init | github.com/charles-lunarg/vk-bootstrap |
| VulkanMemoryAllocator (VMA) | MIT | GPU memory allocation | github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator |
| GLFW | zlib/libpng | window + input | github.com/glfw/glfw |
| miniz | MIT | zip (`.x3pak`) read/write | github.com/richgel999/miniz |
| glm | MIT (Happy Bunny) | math (vectors/matrices) | github.com/g-truc/glm |
| Jolt Physics | MIT | physics + character controller | github.com/jrouwe/JoltPhysics |
| cgltf | MIT | glTF/GLB parsing | github.com/jkuhlmann/cgltf |
| stb (stb_image, stb_image_write) | MIT / public-domain (dual) | image decode + PNG screenshot write | github.com/nothings/stb |
| miniaudio | MIT-0 / public-domain (dual) | audio playback / 3D | github.com/mackron/miniaudio |

## Vendored via CMake FetchContent

| Library | License | Use | Source |
|---|---|---|---|
| llama.cpp + ggml (pinned tag `b9590`) | MIT | in-engine LLM inference (NPC minds), CPU backend only | github.com/ggml-org/llama.cpp |

### LLM model weights (data, NOT in git)

| Model | License | Notes |
|---|---|---|
| Qwen2.5-3B-Instruct Q4_K_M (GGUF) | **Qwen Research License (NON-commercial)** | dev default; download per `assets/models/llm/README.md`. ⚠️ The 3B size is NOT Apache 2.0 (1.5B/7B/14B/32B are) — swap an Apache-2.0 GGUF (e.g. Qwen2.5-1.5B/7B-Instruct) before any commercial ship; the engine loads any `.gguf` in `assets/models/llm/`. |

## Embedded in-tree

| Component | License | Use | Notes |
|---|---|---|---|
| font8x8 (`engine/rhi/font8x8_basic.h`) | public-domain | bitmap HUD/console font | Daniel Hepper's public-domain 8x8 font |
| gif.h (`third_party/gif_h/gif.h`) | public-domain | animated GIF encode (headless `--capture-ai` tool only) | Charlie Tangora's public-domain single-header GIF writer |

## Planned (not yet linked)

| Library | License | Use |
|---|---|---|
| sol3 + Lua / LuaJIT | MIT | scripting VM |
| basis_universal | Apache 2.0 | KTX2 / Basis texture transcode |
| Dear ImGui | MIT | developer UI/tools |
| Tracy | BSD-3-Clause | frame profiler |
| Recast/Detour | zlib | navmesh generation/runtime |

## Rule

Before adding any dependency, confirm its license is MIT / Apache 2.0 / zlib / BSD /
public-domain (or similarly permissive). **Never** add GPL, LGPL, AGPL, MPL-with-
copyleft-concerns, or CC-BY-SA code. When a "Planned" library is actually linked,
move its row up to "Currently linked" and add it to `vcpkg.json`.
