# LLM models (in-engine NPC minds — VIGIL / the HoloTerminal facility AI)

The `.gguf` weights are **NOT committed** (gitignored — multi-GB doesn't belong
in git or LFS). The engine works without them: the HoloTerminal falls back to
canned "SYSTEMS DEGRADED" responses, and `--test-llm` skips the real-model part
of the suite (mock plumbing still runs).

## Get the models

### 7B — GPU-recommended, **Apache 2.0** (commercial-safe)

Qwen2.5-**7B**-Instruct, Q4_K_M quant (~4.7 GB). This is the default when
`ai_gpu=1` — it's far smarter than the 3B and fits comfortably in the RTX 5090's
32 GB VRAM at GPU speed. **Apache 2.0**, so it clears the commercial-license flag
the 3B carries.

```sh
curl -L -o assets/models/llm/qwen2.5-7b-instruct-q4_k_m.gguf \
  "https://huggingface.co/bartowski/Qwen2.5-7B-Instruct-GGUF/resolve/main/Qwen2.5-7B-Instruct-Q4_K_M.gguf"
```

(The `bartowski` single-file quant is used above — the official
`Qwen/Qwen2.5-7B-Instruct-GGUF` Q4_K_M is split into two shards that must be
merged with `llama-gguf-split`. Both derive from the Apache-2.0 7B.)

### 3B — CPU-ok, **NON-commercial license**

Qwen2.5-**3B**-Instruct, Q4_K_M quant (~2.1 GB). Fine on CPU (~17 tok/s) for
development.

> **LICENSE WARNING:** Qwen2.5-**3B** ships under the **Qwen Research License**
> (NON-commercial) — unlike its 1.5B / **7B** / 14B / 32B siblings, which are
> **Apache 2.0**. For any commercial build use the 7B above (or another Apache
> GGUF); the engine loads any `.gguf` in this directory.

```sh
curl -L -o assets/models/llm/qwen2.5-3b-instruct-q4_k_m.gguf \
  "https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF/resolve/main/qwen2.5-3b-instruct-q4_k_m.gguf"
```

## Model-selection rule

Drop any number of `.gguf` files in this directory; the engine picks one by size
based on `ai_gpu`:

- **`ai_gpu=1` (GPU):** loads the **LARGEST** GGUF present (the 7B — smart, and
  the VRAM is there).
- **`ai_gpu=0` (CPU):** loads the **SMALLEST** GGUF present (the 3B — stays
  conversational; a 7B on CPU would crawl).
- If no `.gguf` is found, it looks for the pinned
  `qwen2.5-3b-instruct-q4_k_m.gguf` name.

`--test-llm` always exercises the **largest** present GGUF (so a downloaded 7B is
benchmarked through the GPU path).

## Runtime

- Backend: vendored llama.cpp (FetchContent, tag `b9590`), linked static+PRIVATE
  — llama/ggml types never leak past `engine/llm/LlamaLlmSystem.cpp`.
- **GPU inference:** `GGML_CUDA` is enabled automatically when the **CUDA
  Toolkit (nvcc) >= 12.8** is present at CMake-configure time (12.8+ is required
  for Blackwell / RTX 5090 sm_120). CUDA is a **separate API** from the engine's
  Vulkan device — `GGML_VULKAN` stays OFF, so inference never touches the
  renderer. Without the toolkit the CPU backend builds and `ai_gpu` auto-falls
  back to CPU (one log line); re-run `cmake` after installing the toolkit to
  light up the GPU with zero code changes.
- Cvars: `ai_gpu` (0/1, default 1 → CUDA all-layers with CPU fallback),
  `ai_npc` (0/1), `ai_ctx` (2048), `ai_maxtokens` (256), `ai_temp` (0.7).
- Verify: `X3Engine.exe --test-llm` (runs the real prompt + ai_gpu parity when a
  file exists).
