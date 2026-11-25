import { useState } from "react";
import { Modal } from "./components";
import { placeName, t } from "./i18n";
import { Select } from "./Select";
import { bytes, type BrowserSession, type Storage } from "./types";

export function DirectDownload({
  browser, storage, preferredStorage, download, cancel, view, close,
}: {
  browser: BrowserSession | null;
  storage: Storage[];
  preferredStorage: string;
  download: (url: string, storageId: string, password?: string) => Promise<BrowserSession>;
  cancel: (id: string) => Promise<void>;
  view: (id: string) => void;
  close: () => void;
}) {
  const title = t("Download from URL");
  const [url, setUrl] = useState("");
  const [password, setPassword] = useState("");
  const [drive, setDrive] = useState("");
  const [sending, setSending] = useState(false);
  const [cancelling, setCancelling] = useState(false);
  const [error, setError] = useState("");
  const [started, setStarted] = useState<BrowserSession | null>(() =>
    browser?.directDownload && browser.active ? browser : null,
  );
  const current = browser && browser.id === started?.id ? browser : started;
  const busy = sending || !!browser?.active || !!current?.active;
  const queued = current?.state === "queued";
  const failed = current?.state === "failed";
  const storageId = drive || storage.find(({ id }) => id === preferredStorage)?.id || storage[0]?.id || "";
  const canSubmit = !busy && !!url.trim() && storage.some(({ id }) => id === storageId);

  async function submit() {
    if (!canSubmit) return;
    setSending(true);
    setError("");
    try {
      setStarted(await download(url.trim(), storageId, password || undefined));
    } catch (e) {
      setError(e instanceof Error ? e.message : t("Orbit could not complete this request."));
    } finally {
      setSending(false);
    }
  }

  async function cancelVerification(id: string) {
    setCancelling(true);
    setError("");
    try {
      await cancel(id);
    } catch (e) {
      setError(e instanceof Error ? e.message : t("Orbit could not complete this request."));
    } finally {
      setCancelling(false);
    }
  }

  return (
    <Modal title={title} close={close} className="direct-url-modal">
      <h1>{title}</h1>
      {queued ? (
        <>
          <p role="status">{t(current.message)}</p>
          <div className="dialog-actions">
            <button className="primary" onClick={() => view(current.jobId)}>{t("View download")}</button>
          </div>
        </>
      ) : (
        <>
          <p>{t("Enter a direct HTTP or HTTPS link to a PKG, FPKG, ZIP, 7z or RAR file.")}</p>
          <form onSubmit={(event) => { event.preventDefault(); void submit(); }}>
            <label className="field">
              {t("Download URL")}
              <input
                data-initial-focus={!current?.active ? "" : undefined}
                type="url"
                dir="ltr"
                autoCapitalize="none"
                autoComplete="off"
                spellCheck={false}
                maxLength={2047}
                value={url}
                readOnly={busy}
                required
                onChange={(event) => { setUrl(event.target.value); setStarted(null); setError(""); }}
                onKeyDown={(event) => {
                  if (event.key !== "ArrowDown" || event.shiftKey || event.altKey || event.ctrlKey || event.metaKey) return;
                  event.preventDefault();
                  event.currentTarget.form?.querySelector<HTMLInputElement>('input[type="password"]')?.focus();
                }}
              />
            </label>
            <label className="field">
              {t("Archive password (optional)")}
              <input
                type="password"
                autoComplete="off"
                maxLength={128}
                value={password}
                readOnly={busy}
                onChange={(event) => { setPassword(event.target.value); setError(""); }}
                onKeyDown={(event) => {
                  if (event.key !== "ArrowDown" || event.shiftKey || event.altKey || event.ctrlKey || event.metaKey) return;
                  event.preventDefault();
                  event.currentTarget.form?.querySelector<HTMLButtonElement>('button[role="combobox"]')?.focus();
                }}
              />
            </label>
            <Select
              label={t("Save to")}
              value={storageId}
              options={storage.map((disk) => ({
                value: disk.id,
                label: `${placeName(disk.label)} · ${t("{size} free", { size: bytes(disk.projectedFreeBytes ?? disk.freeBytes) })}`,
              }))}
              onChange={setDrive}
              disabled={busy || !storage.length}
            />
            {error && <p className="form-error" role="alert">{error}</p>}
            {current && <p className={failed ? "form-error" : ""} role={failed ? "alert" : "status"}>{t(current.message)}</p>}
            {!current && browser?.active && <p role="status">{t("Finish or cancel the current verification first.")}</p>}
            {!storage.length && <p role="status">{t("No writable storage is connected.")}</p>}
            {!!storage.length && drive && !storage.some(({ id }) => id === drive) && (
              <p role="status">{t("Reconnect the selected drive or choose another one.")}</p>
            )}
            <div className="dialog-actions">
              <button type="submit" className="primary" disabled={!canSubmit}>
                {sending || current?.active ? t("Checking your file") : t("Download")}
              </button>
              {current?.active && current.state !== "cancelled" && (
                <button type="button" disabled={cancelling} onClick={() => void cancelVerification(current.id)}>
                  {cancelling ? t("Cancelling…") : t("Cancel verification")}
                </button>
              )}
            </div>
          </form>
        </>
      )}
    </Modal>
  );
}
