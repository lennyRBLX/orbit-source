import { useEffect, useRef, useState, type ImgHTMLAttributes } from "react";
import { acquireArtwork, type ArtKind } from "./artworkCache";
import { watchArtwork } from "./artworkVisibility";

type Props = Omit<
  ImgHTMLAttributes<HTMLImageElement>,
  "src" | "loading" | "fetchPriority"
> & {
  src: string;
  fallbackSrc?: string | null;
  priority?: boolean;
  kind?: ArtKind;
  imageRef?: { current: HTMLImageElement | null };
};
export function Artwork(props: Props) {
  // A catalogue update starts a fresh attempt, even when this tile stays mounted.
  return (
    <ArtworkImage key={`${props.src}\n${props.fallbackSrc || ""}\n${props.kind || "cover"}`} {...props} />
  );
}
function ArtworkImage({ src, fallbackSrc, priority = false, kind = "cover", imageRef, onError, ...props }: Props) {
  const ref = useRef<HTMLImageElement>(null);
  const [visible, setVisible] = useState(priority);
  const [attempt, setAttempt] = useState(0);
  const [resolved, setResolved] = useState<string>();
  const load = priority || visible;
  useEffect(() => {
    if (!load && ref.current)
      return watchArtwork(ref.current, () => setVisible(true));
  }, [load]);
  useEffect(() => {
    if (!load) return;
    if (attempt > 1) { setResolved("/orbit.svg"); return; }
    const source = attempt ? fallbackSrc || src : src;
    if (!source.startsWith("https://")) { setResolved(source); return; }
    let alive = true;
    const request = acquireArtwork(source, attempt ? null : fallbackSrc, kind);
    request.ready.then(url => { if (alive) setResolved(url); }, () => {
      if (alive) setResolved("/orbit.svg");
    });
    return () => { alive = false; request.release(); };
  }, [load, src, fallbackSrc, kind, attempt]);
  return (
    <img
      {...props}
      ref={node => { ref.current = node; if (imageRef) imageRef.current = node; }}
      src={load ? resolved : undefined}
      loading="eager"
      fetchPriority={priority ? "high" : "auto"}
      onError={event => {
        if (onError) onError(event);
        else if (!attempt && fallbackSrc && fallbackSrc !== src) setAttempt(1);
        else { setAttempt(2); setResolved("/orbit.svg"); }
      }}
    />
  );
}
