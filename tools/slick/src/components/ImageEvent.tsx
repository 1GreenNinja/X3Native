// ImageEvent.tsx — inline full-res image rendering with click-to-expand
// lightbox (PR-4). Renders from the authenticated full-res download endpoint,
// NEVER the thumbnail — Tim's explicit ask: "HIGH quality image sharing."

import { useState } from "preact/hooks";
import { mxcToUrl } from "../client";

export function ImageEvent({ content }: { content: Record<string, any> }) {
  const [open, setOpen] = useState(false);
  const mxc: string | undefined = content.url ?? content.file?.url;
  if (!mxc) return <div class="msg-body image-chip">🖼️ {content.body ?? "image"} <em>(no url)</em></div>;

  const fullUrl = mxcToUrl(mxc);
  const info = content.info ?? {};
  // Constrain display height in-stream via CSS; the fetched asset is the original.
  const aspect = info.w && info.h ? info.w / info.h : undefined;

  return (
    <>
      <img
        class="inline-image"
        src={fullUrl}
        alt={content.body ?? "image"}
        loading="lazy"
        style={aspect ? { aspectRatio: String(aspect) } : undefined}
        onClick={() => setOpen(true)}
      />
      {open && (
        <div class="lightbox" onClick={() => setOpen(false)}>
          <img class="lightbox-img" src={fullUrl} alt={content.body ?? "image"} />
          <div class="lightbox-bar" onClick={(e) => e.stopPropagation()}>
            <span class="lightbox-name">{content.body ?? "image"}</span>
            <a class="lightbox-dl" href={fullUrl} download={content.body ?? "image"} target="_blank" rel="noreferrer">
              Download original
            </a>
            <button class="lightbox-close" onClick={() => setOpen(false)}>✕</button>
          </div>
        </div>
      )}
    </>
  );
}
