# LLM models (in-engine NPC minds)

The `.gguf` weights are **NOT committed** (gitignored — ~2 GB doesn't belong in
git or LFS). The engine works without them: the HoloTerminal falls back to
canned "SYSTEMS DEGRADED" responses, and `--test-llm` skips the real-model part
of the suite (mock plumbing still runs).

## Get the model

Qwen2.5-3B-Instruct, Q4_K_M quant:

> **LICENSE WARNING:** Qwen2.5-**3B** ships under the **Qwen Research License**
> (NON-commercial) — unlike its 1.5B/7B/14B/32B siblings, which are Apache 2.0.
> Fine for development; for a commercial build swap in an Apache-2.0 GGUF
> (e.g. `Qwen/Qwen2.5-1.5B-Instruct-GGUF` or `Qwen2.5-7B-Instruct-GGUF`) —
> the engine loads ANY `.gguf` dropped into this directory when the default
> filename is absent.

```sh
curl -L -o assets/models/llm/qwen2.5-3b-instruct-q4_k_m.gguf \
  "https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF/resolve/main/qwen2.5-3b-instruct-q4_k_m.gguf"
```

(~2.1 GB. Any Qwen2.5-3B-Instruct Q4 GGUF mirror works — e.g.
`bartowski/Qwen2.5-3B-Instruct-GGUF` — but keep the filename above; the engine
resolves `<assetRoot>/models/llm/qwen2.5-3b-instruct-q4_k_m.gguf`.)

## Runtime

- Backend: vendored llama.cpp (FetchContent, tag `b9590`), **CPU inference
  only** for v1 (`GGML_VULKAN OFF` — never touches the engine's Vulkan device).
- Cvars: `ai_npc` (0/1), `ai_ctx` (2048), `ai_maxtokens` (256), `ai_temp` (0.7).
- Verify: `X3Engine.exe --test-llm` (runs a real prompt when the file exists).
