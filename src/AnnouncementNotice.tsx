import { useEffect, useState } from "react";
import { api } from "./api";
import { language, t } from "./i18n";

// What the owner posts in announcements.json (docs/announcements.md), as the PS5 passes it on:
// only the ones that show now, for this version.
export interface Announcement {
  id: string;
  title: string;
  message?: string;
  translations?: Record<string, { title: string; message?: string }>;
}
const seenKey = "orbit.announcements-seen.v1";
function readSeen(): string[] {
  try {
    const value = JSON.parse(localStorage.getItem(seenKey) || "[]");
    return Array.isArray(value) ? value.filter((id) => typeof id === "string") : [];
  } catch {
    return [];
  }
}
// In the viewer's language, then its base language ("pt" for "pt-BR"), then as written.
export function localized(a: Announcement, code: string = language()) {
  const text = a.translations?.[code] || a.translations?.[code.split("-")[0]];
  return text ? { title: text.title, message: text.message } : { title: a.title, message: a.message };
}
// One announcement at a time, until it is closed. None, an unreadable file or an unreachable
// PS5 shows nothing.
export function AnnouncementNotice({ enabled, hidden }: { enabled: boolean; hidden: boolean }) {
  const [list, setList] = useState<Announcement[]>([]);
  const [seen, setSeen] = useState(readSeen);
  useEffect(() => {
    if (!enabled) {
      setList([]);
      return;
    }
    let active = true;
    let retry: ReturnType<typeof setTimeout> | undefined;
    const refresh = async () => {
      clearTimeout(retry);
      if (document.visibilityState === "hidden") return;
      try {
        const next = await api<{ announcements?: Announcement[]; refreshing?: boolean }>("/announcements");
        if (active) {
          setList(Array.isArray(next.announcements) ? next.announcements : []);
          if (next.refreshing) retry = setTimeout(() => void refresh(), 1_000);
        }
      } catch {
        // An older PS5 service, or no connection: nothing to announce.
      }
    };
    void refresh();
    const timer = setInterval(() => void refresh(), 10 * 60_000);
    return () => {
      active = false;
      clearInterval(timer);
      clearTimeout(retry);
    };
  }, [enabled]);
  const current = list.find((a) => a && typeof a.id === "string" && typeof a.title === "string" && !seen.includes(a.id));
  if (!enabled || hidden || !current) return null;
  const { title, message } = localized(current);
  const close = () => {
    // The newest 50 are enough: the file holds 10 at most.
    const next = [current.id, ...seen.filter((id) => id !== current.id)].slice(0, 50);
    setSeen(next);
    try {
      localStorage.setItem(seenKey, JSON.stringify(next));
    } catch {
      /* This view still forgets it. */
    }
  };
  return (
    <aside className="update-notice" aria-label={title} role="status">
      <div>
        <strong>{title}</strong>
        {message && <p>{message}</p>}
      </div>
      <div className="update-notice-actions">
        <button onClick={close}>{t("Dismiss")}</button>
      </div>
    </aside>
  );
}
