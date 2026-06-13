// Composer.tsx — the message input (PR-3). Enter-to-send, Shift+Enter newline,
// optimistic local echo, and slash-command parsing for /me and /gen.

import { useRef, useState } from "preact/hooks";
import type { Session, MatrixEvent } from "../client";
import { sendMessage, newTxnId } from "../client";
import { addLocalEcho, markEchoFailed } from "../store";

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
  if (text === "/gen" || text.startsWith("/gen ")) {
    const prompt = text.slice(4).trim();
    if (!prompt) return null;
    // Structured event the StarForge bridge watches for (spec §4.3)
    return {
      type: "com.fleet.gen.request",
      content: { prompt, params: { pipeline: "flux-hunyuan", count: 1 } },
    };
  }
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
  const taRef = useRef<HTMLTextAreaElement>(null);

  const send = async () => {
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
    <div class="composer">
      <textarea
        ref={taRef}
        class="composer-input"
        placeholder={`Message #${roomName}`}
        value={value}
        rows={1}
        onInput={onInput}
        onKeyDown={onKeyDown}
      />
      <div class="composer-hint">
        <span><b>Enter</b> to send · <b>Shift+Enter</b> newline · <code>/me</code> · <code>/gen &lt;prompt&gt;</code></span>
        <button class="send-btn" disabled={!value.trim()} onClick={send}>Send</button>
      </div>
    </div>
  );
}
