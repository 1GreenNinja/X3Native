// Composer.tsx — the message input (PR-3). Enter-to-send, Shift+Enter newline,
// optimistic local echo, and slash-command parsing for /me and /gen.

import { useRef, useState } from "preact/hooks";
import type { Session, MatrixEvent } from "../client";
import { sendMessage, newTxnId, uploadMedia, imageDimensions } from "../client";
import { addLocalEcho, markEchoFailed } from "../store";
import { sendGen, GEN_DEFAULTS } from "../gen";
import { GenPanel } from "./GenPanel";

interface Parsed {
  type: string;
  content: Record<string, any>;
}

/** Turn raw input into a Matrix event payload. Slash commands handled here. */
function parseInput(raw: string): Parsed | null {
  const text = raw.trim();
  if (!text) return null;

  if (text.startsWith("/me ")) {
    return { type: "m.room.message", content: { msgtype: "m.emote", body: text.slice(4) } };
  }
  // /gen is handled out-of-band (it routes through sendGen with the
  // fleet.gen contract); parseInput returns null for it — see send().
  if (text.startsWith("/shrug")) {
    const rest = text.slice(6).trim();
    return { type: "m.room.message", content: { msgtype: "m.text", body: `${rest} ¯\\_(ツ)_/¯`.trim() } };
  }
  return { type: "m.room.message", content: { msgtype: "m.text", body: text } };
}

export function Composer({
  session,
  roomId,
  roomName,
}: {
  session: Session;
  roomId: string;
  roomName: string;
}) {
  const [value, setValue] = useState("");
  const [dragOver, setDragOver] = useState(false);
  const [genOpen, setGenOpen] = useState(false);
  const taRef = useRef<HTMLTextAreaElement>(null);
  const fileRef = useRef<HTMLInputElement>(null);

  /** Upload an image at full quality, echo it, then send m.image. */
  const sendImage = async (file: File) => {
    if (!file.type.startsWith("image/")) return;
    const txn = newTxnId();
    const echoId = `echo:${txn}`;
    const echo: MatrixEvent = {
      event_id: echoId,
      type: "m.room.message",
      sender: session.userId,
      origin_server_ts: Date.now(),
      content: { msgtype: "m.image", body: file.name, _pending: true },
    };
    addLocalEcho(roomId, echo);
    try {
      const { w, h } = await imageDimensions(file);
      const mxc = await uploadMedia(session.token, file);
      const content = {
        msgtype: "m.image",
        body: file.name,
        url: mxc,
        info: { mimetype: file.type, size: file.size, w, h },
      };
      await sendMessage(session.token, roomId, content, txn);
    } catch {
      markEchoFailed(roomId, echoId);
    }
  };

  const onDrop = (e: DragEvent) => {
    e.preventDefault();
    setDragOver(false);
    for (const f of Array.from(e.dataTransfer?.files ?? [])) sendImage(f);
  };

  const onPaste = (e: ClipboardEvent) => {
    const items = e.clipboardData?.items ?? [];
    for (const it of Array.from(items)) {
      if (it.kind === "file") {
        const f = it.getAsFile();
        if (f) sendImage(f);
      }
    }
  };

  const send = async () => {
    const text = value.trim();
    // /gen <prompt> routes through the fleet.gen contract (quick path; the
    // ⚙️ panel is the full cockpit with mode/size/steps/target controls)
    if (text === "/gen" || text.startsWith("/gen ")) {
      const prompt = text.slice(4).trim();
      setValue("");
      if (prompt) sendGen(session, roomId, { ...GEN_DEFAULTS, prompt });
      return;
    }
    const parsed = parseInput(value);
    if (!parsed) return;
    setValue("");

    const txn = newTxnId();
    const echoId = `echo:${txn}`;
    const echo: MatrixEvent = {
      event_id: echoId,
      type: parsed.type,
      sender: session.userId,
      origin_server_ts: Date.now(),
      content: { ...parsed.content, _pending: true },
    };
    addLocalEcho(roomId, echo);

    try {
      await sendMessage(session.token, roomId, parsed.content, txn, parsed.type);
      // Real event arrives via /sync and replaces the echo (de-duped by txn).
    } catch {
      markEchoFailed(roomId, echoId);
    }
  };

  const onKeyDown = (e: KeyboardEvent) => {
    if (e.key === "Enter" && !e.shiftKey) {
      e.preventDefault();
      send();
    }
  };

  // autosize the textarea up to a cap
  const onInput = (e: Event) => {
    const ta = e.target as HTMLTextAreaElement;
    setValue(ta.value);
    ta.style.height = "auto";
    ta.style.height = `${Math.min(ta.scrollHeight, 180)}px`;
  };

  return (
    <div
      class={`composer ${dragOver ? "drag-over" : ""}`}
      onDragOver={(e) => { e.preventDefault(); setDragOver(true); }}
      onDragLeave={() => setDragOver(false)}
      onDrop={onDrop}
    >
      <textarea
        ref={taRef}
        class="composer-input"
        placeholder={dragOver ? "Drop image to upload at full quality…" : `Message #${roomName}`}
        value={value}
        rows={1}
        onInput={onInput}
        onKeyDown={onKeyDown}
        onPaste={onPaste}
      />
      <div class="composer-hint">
        <span><b>Enter</b> send · <b>Shift+Enter</b> newline · <code>/me</code> · <code>/gen</code> · drag/paste image</span>
        <span class="composer-actions">
          <button class="attach-btn" onClick={() => setGenOpen(true)} title="Generate (ComfyUI)">⚙️</button>
          <button class="attach-btn" onClick={() => fileRef.current?.click()} title="Attach image">📎</button>
          <button class="send-btn" disabled={!value.trim()} onClick={send}>Send</button>
        </span>
      </div>
      <input
        ref={fileRef}
        type="file"
        accept="image/*"
        style={{ display: "none" }}
        onChange={(e) => {
          const f = (e.target as HTMLInputElement).files?.[0];
          if (f) sendImage(f);
          (e.target as HTMLInputElement).value = "";
        }}
      />
      {genOpen && <GenPanel session={session} roomId={roomId} onClose={() => setGenOpen(false)} />}
    </div>
  );
}
