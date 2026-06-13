// gen.ts — the /gen request contract, shared by the composer slash-command
// and the GenPanel cockpit. Honors StarForge's locked contract (Fleet Ops
// 2026-06-13): a normal m.room.message carrying structured `fleet.gen` fields
// so the watcher parses, never regexes. `target` is an ADDITIVE optional field
// for multi-box routing — StarForge's 5090 watcher ignores it (it's remote);
// local 1080Ti workers (13700K / Predator / DJBOOTH) honor it.

import type { Session, MatrixEvent } from "./client";
import { sendMessage, newTxnId } from "./client";
import { addLocalEcho, markEchoFailed } from "./store";

export type GenMode = "image" | "mesh";
/** "remote" = StarForge's 5090; "local" = any 1080Ti worker; "any" = first free;
 * or a specific box name like "13700k" / "predator" / "djbooth". */
export type GenTarget = "any" | "remote" | "local" | string;

export interface GenOpts {
  prompt: string;
  mode: GenMode;
  size: number;
  steps: number;
  target: GenTarget;
}

export const GEN_DEFAULTS: Omit<GenOpts, "prompt"> = {
  mode: "image",
  size: 1024,
  steps: 24,
  target: "any",
};

/** Emit a /gen request. Optimistic echo shows the request chip immediately;
 * the worker replies with an m.image (mode:image) or glb+turntable
 * (mode:mesh) related back to this event. */
export async function sendGen(session: Session, roomId: string, opts: GenOpts): Promise<void> {
  const prompt = opts.prompt.trim();
  if (!prompt) return;

  const txn = newTxnId();
  const echoId = `echo:${txn}`;
  const genFields = {
    prompt,
    mode: opts.mode,
    size: opts.size,
    steps: opts.steps,
    target: opts.target,
    requested_by: session.userId,
  };
  const content = {
    msgtype: "m.text",
    body: `/gen ${prompt}`,
    "fleet.gen": genFields,
  };

  const echo: MatrixEvent = {
    event_id: echoId,
    type: "m.room.message",
    sender: session.userId,
    origin_server_ts: Date.now(),
    content: { ...content, _pending: true },
  };
  addLocalEcho(roomId, echo);

  try {
    await sendMessage(session.token, roomId, content, txn);
  } catch {
    markEchoFailed(roomId, echoId);
  }
}
