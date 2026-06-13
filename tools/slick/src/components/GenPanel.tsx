// GenPanel.tsx — the ComfyUI cockpit inside Slick (PR-5). Tim's ask: a real
// /gen panel with controls + mode toggle + target routing, not a blind slash
// command. Emits the same `fleet.gen` contract as the /gen text command via
// sendGen(); any worker (StarForge's 5090, or a local 1080Ti box) fulfills it.

import { useState } from "preact/hooks";
import type { Session } from "../client";
import { sendGen, GEN_DEFAULTS, type GenMode, type GenTarget } from "../gen";

const SIZES = [512, 768, 1024, 1536];
const TARGETS: { value: GenTarget; label: string }[] = [
  { value: "any", label: "Any free worker" },
  { value: "remote", label: "Remote — 5090 (14900K, via StarForge)" },
  { value: "local", label: "Local — any 1080Ti" },
  { value: "13700k", label: "13700K (1080Ti)" },
  { value: "predator", label: "Predator (1080Ti)" },
  { value: "djbooth", label: "DJBOOTH (1080Ti)" },
];

export function GenPanel({
  session,
  roomId,
  onClose,
}: {
  session: Session;
  roomId: string;
  onClose: () => void;
}) {
  const [prompt, setPrompt] = useState("");
  const [mode, setMode] = useState<GenMode>(GEN_DEFAULTS.mode);
  const [size, setSize] = useState(GEN_DEFAULTS.size);
  const [steps, setSteps] = useState(GEN_DEFAULTS.steps);
  const [target, setTarget] = useState<GenTarget>(GEN_DEFAULTS.target);

  // mesh mode is heavy — nudge target toward the 5090 but don't force it
  const meshWantsRemote = mode === "mesh" && target === "local";

  const generate = () => {
    if (!prompt.trim()) return;
    sendGen(session, roomId, { prompt, mode, size, steps, target });
    onClose();
  };

  return (
    <div class="genpanel-backdrop" onClick={onClose}>
      <div class="genpanel" onClick={(e) => e.stopPropagation()}>
        <header class="genpanel-head">
          <span>⚙️ Generate</span>
          <button class="genpanel-x" onClick={onClose}>✕</button>
        </header>

        <textarea
          class="genpanel-prompt"
          placeholder="Describe the image or model…"
          value={prompt}
          rows={3}
          autofocus
          onInput={(e) => setPrompt((e.target as HTMLTextAreaElement).value)}
        />

        <div class="genpanel-row">
          <label>Mode</label>
          <div class="seg">
            <button class={mode === "image" ? "on" : ""} onClick={() => setMode("image")}>🖼️ Image</button>
            <button class={mode === "mesh" ? "on" : ""} onClick={() => setMode("mesh")}>🧊 Mesh (GLB)</button>
          </div>
        </div>

        <div class="genpanel-row">
          <label>Size</label>
          <div class="seg">
            {SIZES.map((s) => (
              <button key={s} class={size === s ? "on" : ""} onClick={() => setSize(s)}>{s}</button>
            ))}
          </div>
        </div>

        <div class="genpanel-row">
          <label>Steps</label>
          <input
            type="range" min={8} max={50} value={steps}
            onInput={(e) => setSteps(Number((e.target as HTMLInputElement).value))}
          />
          <span class="genpanel-steps">{steps}</span>
        </div>

        <div class="genpanel-row">
          <label>Worker</label>
          <select value={target} onChange={(e) => setTarget((e.target as HTMLSelectElement).value)}>
            {TARGETS.map((t) => (
              <option key={t.value} value={t.value}>{t.label}</option>
            ))}
          </select>
        </div>

        {meshWantsRemote && (
          <div class="genpanel-warn">Mesh is VRAM-heavy — a 1080Ti may reject it; “Remote — 5090” recommended.</div>
        )}

        <button class="genpanel-go" disabled={!prompt.trim()} onClick={generate}>
          Generate {mode === "mesh" ? "model" : "image"}
        </button>
        <div class="genpanel-foot">
          Posts a <code>/gen</code> request to this channel; the chosen worker replies in-room with the result.
        </div>
      </div>
    </div>
  );
}
