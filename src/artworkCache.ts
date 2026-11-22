import { fetchArtwork } from "./api";

export type ArtKind = "cover" | "hero" | "thumbnail";
type Entry = { refs: number; bytes: number; url?: string; controller: AbortController; ready: Promise<string> };
const entries = new Map<string, Entry>();
const memoryLimit = 32 * 1024 * 1024;
function trim() {
  let bytes = [...entries.values()].reduce((sum, entry) => sum + entry.bytes, 0);
  for (const [key, entry] of entries) {
    if (bytes <= memoryLimit && entries.size <= 128) break;
    if (entry.refs) continue;
    entries.delete(key); bytes -= entry.bytes;
    if (entry.url) URL.revokeObjectURL(entry.url);
  }
}
// Waits longer each time Orbit is still fetching a picture: 0.6 s, then up to 5 s.
function wait(signal: AbortSignal, attempt: number) {
  return new Promise<void>((resolve, reject) => {
    const abort = () => { clearTimeout(timer); reject(new Error("Artwork cancelled")); };
    const timer = setTimeout(() => { signal.removeEventListener("abort", abort); resolve(); }, Math.min(5000, 600 * 1.5 ** attempt));
    signal.addEventListener("abort", abort, { once: true });
    if (signal.aborted) abort();
  });
}
// A browser opens about six connections to Orbit. Pictures use at most three,
// so the game list and download progress never wait behind a screen of covers.
const slots = { active: 0, waiting: [] as (() => void)[] };
// Undefined when a slot was free, so the first request starts without a wait.
function takeSlot(signal: AbortSignal): Promise<void> | undefined {
  if (slots.active < 3) { slots.active++; return undefined; }
  return new Promise<void>((resolve, reject) => {
    const go = () => { signal.removeEventListener("abort", abort); slots.active++; resolve(); };
    const abort = () => {
      const at = slots.waiting.indexOf(go);
      if (at >= 0) slots.waiting.splice(at, 1);
      reject(new Error("Artwork cancelled"));
    };
    slots.waiting.push(go);
    signal.addEventListener("abort", abort, { once: true });
    if (signal.aborted) abort();
  });
}
function freeSlot() {
  slots.active--;
  slots.waiting.shift()?.();
}
export function acquireArtwork(src: string, fallback: string | null | undefined, kind: ArtKind) {
  const query = new URLSearchParams({ src, kind });
  if (fallback?.startsWith("https://") && fallback !== src) query.set("fallback", fallback);
  const key = query.toString();
  let entry = entries.get(key);
  if (!entry) {
    const controller = new AbortController();
    entry = { refs: 0, bytes: 0, controller, ready: Promise.resolve("") };
    const current = entry;
    entries.set(key, current);
    const timeout = setTimeout(() => controller.abort(), 60000);
    current.ready = (async () => {
      // Backend fetches continue independently of a particular tile. Poll only
      // while a visible component needs them; no browser connection waits on a CDN.
      for (let attempt = 0; attempt < 100; attempt++) {
        const queued = takeSlot(controller.signal);
        if (queued) await queued;
        let blob: Blob | null = null;
        try {
          const response = await fetchArtwork(query, controller.signal);
          if ([202, 401, 503].includes(response.status)) await response.arrayBuffer();
          else if (!response.ok || !/^image\/(png|jpeg|webp|gif|avif)$/.test(response.headers.get("Content-Type") || ""))
            throw new Error("Artwork unavailable");
          else blob = await response.blob();
        } finally { freeSlot(); }
        if (!blob) { await wait(controller.signal, attempt); continue; }
        if (controller.signal.aborted) throw new Error("Artwork cancelled");
        current.bytes = blob.size; current.url = URL.createObjectURL(blob);
        trim(); return current.url;
      }
      throw new Error("Artwork unavailable");
    })().catch(error => { if (entries.get(key) === current) entries.delete(key); throw error; }).finally(() => clearTimeout(timeout));
  } else { entries.delete(key); entries.set(key, entry); }
  entry.refs++;
  let released = false;
  return {
    ready: entry.ready,
    release() {
      if (released) return;
      released = true; entry.refs--;
      if (!entry.refs && !entry.url) {
        entry.controller.abort();
        if (entries.get(key) === entry) entries.delete(key);
      }
      trim();
    },
  };
}
