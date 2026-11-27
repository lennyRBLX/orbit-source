// Download from links (POST /api/v1/links): files outside the catalogue, from a
// direct link the user pastes. The PS5 checks each link, then they join Downloads
// like any game. Also gives a paused or failed link download a fresh link
// (POST /api/v1/downloads/<id>/replace-link), keeping the part already downloaded.
import { useEffect, useRef, useState } from "react";
import { api } from "./api";
import { Modal } from "./components";
import { Select } from "./Select";
import { bytes, type Job, type Storage } from "./types";
import { placeName, t } from "./i18n";

export interface LinkItem {
  host: string;
  filename: string;
  state: "waiting" | "checking" | "queued" | "failed";
  message: string;
  jobId: string;
}
export interface LinksStatus {
  id: string;
  state: "idle" | "checking" | "done";
  replaceJobId: string;
  items: LinkItem[];
}

export const MAX_LINKS = 20;

// One link per line; blank lines and the spaces around a link don't count.
export function parseLinks(text: string): string[] {
  return text.split(/\r?\n/).map((line) => line.trim()).filter(Boolean);
}

// What stops these links being sent, in the PS5's words; "" when they can go.
export function linkProblem(links: string[]): string {
  if (!links.length) return t("Paste at least one link.");
  if (links.length > MAX_LINKS) return t("Add up to 20 links at a time.");
  for (let i = 0; i < links.length; i++) {
    if (!/^https:\/\/[^\s#]+$/i.test(links[i]))
      return t("Link {number} isn't a download link that starts with https://.", { number: i + 1 });
    const first = links.indexOf(links[i]);
    if (first < i) return t("Link {number} is the same as link {other}.", { number: i + 1, other: first + 1 });
  }
  return "";
}

const stateLabels: Record<LinkItem["state"], string> = {
  waiting: /* i18n */ "Waiting",
  checking: /* i18n */ "Checking…",
  queued: /* i18n */ "Added",
  failed: /* i18n */ "Not added",
};

export function AddLinks({
  drives,
  preferred,
  replace,
  close,
  refresh,
}: {
  drives: Storage[];
  preferred?: string;
  // A paused or failed link download that takes a fresh link.
  replace?: Job;
  close: () => void;
  refresh: () => Promise<unknown>;
}) {
  const [text, setText] = useState("");
  const [storageId, setStorageId] = useState(
    drives.find((d) => d.id === preferred)?.id || drives[0]?.id || "",
  );
  const [status, setStatus] = useState<LinksStatus | null>(null);
  const [error, setError] = useState("");
  const [sending, setSending] = useState(false);
  const alive = useRef(true);
  useEffect(() => () => { alive.current = false; }, []);
  const checking = sending || status?.state === "checking";
  const links = parseLinks(text);

  // Follows the PS5's check of this batch until every link is added or refused.
  async function follow(batch: string) {
    while (alive.current) {
      await new Promise((resolve) => setTimeout(resolve, 700));
      if (!alive.current) return;
      const next = await api<LinksStatus>("/links").catch(() => null);
      if (!alive.current) return;
      if (!next || next.id !== batch) continue;
      setStatus(next);
      void refresh().catch(() => {});
      if (next.state !== "checking") return;
    }
  }
  async function submit() {
    const problem = replace ? linkProblem(links.slice(0, 1)) : linkProblem(links);
    if (problem) { setError(problem); return; }
    if (replace && links.length > 1) { setError(t("Paste one link.")); return; }
    if (!replace && !storageId) { setError(t("Connect a drive first.")); return; }
    setSending(true);
    setError("");
    try {
      const started = replace
        ? await api<LinksStatus>(`/downloads/${replace.id}/replace-link`, { url: links[0] })
        : await api<LinksStatus>("/links", { urls: links, storageId });
      setStatus(started);
      setSending(false);
      await follow(started.id);
    } catch (e) {
      setError(t((e as Error).message));
      setSending(false);
    }
  }
  const title = replace ? t("Replace link") : t("Download from links");
  const added = status?.items.filter((i) => i.state === "queued").length || 0;
  return (
    <Modal title={title} className="add-links" close={() => { if (!sending) close(); }}>
      <form className="archive-password-form add-links-form" onSubmit={(e) => { e.preventDefault(); if (!status) void submit(); }}>
        <h2>{title}</h2>
        {!status ? (
          <>
            <p>
              {replace
                ? t("Paste a fresh link to the same file. The part already downloaded is kept.")
                : t("Paste direct download links, one per line, up to 20. ZIP, 7z and RAR files unpack like games.")}
            </p>
            {replace && <p className="fine">{replace.title || replace.filename}</p>}
            <label>
              {replace ? t("New link") : t("Links")}
              <textarea
                data-initial-focus
                dir="ltr"
                rows={replace ? 2 : 6}
                spellCheck={false}
                autoComplete="off"
                placeholder="https://"
                value={text}
                disabled={checking}
                onChange={(e) => { setText(e.target.value); setError(""); }}
              />
            </label>
            {!replace && (
              <Select
                label={t("Destination drive")}
                value={storageId}
                onChange={setStorageId}
                disabled={checking}
                options={drives.map((d) => ({
                  value: d.id,
                  label: `${placeName(d.label)} · ${t("{size} free", { size: bytes(d.freeBytes) })}`,
                }))}
              />
            )}
            {error && <p className="job-error" role="alert">{error}</p>}
            <div className="actions">
              <button className="primary" type="submit" disabled={checking || !links.length}>
                {replace ? t("Replace link") : t("Download")}
              </button>
              <button type="button" disabled={sending} onClick={close}>{t("Cancel")}</button>
            </div>
          </>
        ) : (
          <>
            <p role="status">
              {status.state === "checking"
                ? t("Checking each link before adding it to Downloads…")
                : replace
                  ? status.items[0]?.state === "queued" ? t("Link replaced. The download continues.") : t("The link wasn't replaced.")
                  : t("{added} of {total} added to Downloads.", { added, total: status.items.length })}
            </p>
            <ol className="link-results">
              {status.items.map((item, i) => (
                <li key={i} className={`link-result ${item.state}`}>
                  <strong dir="auto">{item.filename || item.host}</strong>
                  <span className={`status ${item.state === "failed" ? "error" : item.state === "queued" ? "complete" : "queued"}`}>
                    {t(stateLabels[item.state])}
                  </span>
                  {item.state === "failed" && item.message && <small className="job-error">{t(item.message)}</small>}
                  {item.filename && item.host && <small dir="ltr">{item.host}</small>}
                </li>
              ))}
            </ol>
            <div className="actions">
              <button className="primary" type="button" disabled={status.state === "checking"} onClick={close} data-initial-focus>
                {t("Done")}
              </button>
              {!replace && status.state !== "checking" && (
                <button type="button" onClick={() => { setStatus(null); setText(""); }}>
                  {t("Add more links")}
                </button>
              )}
            </div>
          </>
        )}
      </form>
    </Modal>
  );
}
