import { useCallback, useEffect, useRef, useState } from "react";
import { Icon } from "./components";
import type { RemoteSource, RemoteSources, SourceId, SourceSettings } from "./types";
import { ApiError, addRemoteSource, enableRemoteSource, refreshRemoteSources, remoteSources, removeRemoteSource } from "./api";
import { formatDate, formatNumber, t, tn } from "./i18n";

export function Sources({
  settings,
  paired,
  pair,
  save,
  saved,
  rows = false,
}: {
  settings: SourceSettings;
  paired: boolean;
  pair: () => void;
  save: (enabled: SourceId[], noticeVersion: number) => Promise<void>;
  saved?: () => void;
  // Crossbar, Cinema and Panorama list each source as a row with a switch.
  rows?: boolean;
}) {
  const [selected, setSelected] = useState<SourceId[]>(settings.enabled),
    [acknowledged, setAcknowledged] = useState(settings.acknowledged),
    [pending, setPending] = useState(false),
    [error, setError] = useState("");
  const form = useRef<HTMLFormElement>(null);
  useEffect(() => {
    form.current?.querySelector<HTMLButtonElement>(".source-card, .source-row")?.focus();
  }, []);
  async function submit() {
    setPending(true);
    setError("");
    try {
      await save(selected, settings.noticeVersion);
      saved?.();
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setPending(false);
    }
  }
  return (
    <form
      ref={form}
      className="sources-form"
      onSubmit={(e) => {
        e.preventDefault();
        if (paired) void submit();
        else pair();
      }}
    >
      <h1>
        {settings.acknowledged
          ? t("Download sources")
          : t("Choose your sources")}
      </h1>
      <p className="source-intro">
        {t(
          "Select your download sources. You can change them anytime.",
        )}
      </p>
      {rows ? (
        <div className="source-rows" role="group" aria-label={t("Download sources")}>
          {settings.options.map((source) => {
            const checked = selected.includes(source.id);
            return (
              <button
                key={source.id}
                type="button"
                role="switch"
                aria-checked={checked}
                className="source-row"
                disabled={pending}
                onClick={() =>
                  setSelected((current) =>
                    current.includes(source.id) ? current.filter((id) => id !== source.id) : [...current, source.id],
                  )
                }
              >
                <span className="source-row-text">
                  <strong>{t(source.label)}</strong>
                  <small>
                    {tn(source.releaseCount, "{count} download option", "{count} download options", {
                      count: formatNumber(source.releaseCount),
                    })}
                  </small>
                </span>
                <span className="source-switch-state" aria-hidden="true">
                  {checked ? t("On") : t("Off")}
                </span>
                <span className="source-switch" aria-hidden="true" />
              </button>
            );
          })}
        </div>
      ) : (
      <div
        className="source-grid"
        role="group"
        aria-label={t("Download sources")}
      >
        {settings.options.map((source) => {
          const checked = selected.includes(source.id);
          return (
            <button
              key={source.id}
              type="button"
              role="checkbox"
              aria-checked={checked}
              aria-label={t(source.label)}
              className={`source-card ${checked ? "selected" : ""}`}
              disabled={pending}
              onClick={() =>
                setSelected((current) =>
                  current.includes(source.id)
                    ? current.filter((id) => id !== source.id)
                    : [...current, source.id],
                )
              }
            >
              <span className="choice-check" aria-hidden="true">
                {checked && <Icon name="check" />}
              </span>
              {t(source.label)}
            </button>
          );
        })}
      </div>
      )}
      <div className="source-notice">
        <h2>{t("Before you download")}</h2>
        <p>
          {t(
            "You are responsible for checking that your downloads are lawful. Third-party downloads are at your own risk. Only download content you have permission to access and use, following applicable law and the provider’s terms. Public links do not prove permission, and Orbit does not guarantee the files’ safety or authenticity.",
          )}
        </p>
      </div>
      <button
        type="button"
        role="checkbox"
        aria-checked={acknowledged}
        className="source-acknowledgement"
        disabled={pending}
        onClick={() => setAcknowledged((current) => !current)}
      >
        <span className="choice-check" aria-hidden="true">
          {acknowledged && <Icon name="check" />}
        </span>
        <span>
          {t(
            "I understand the risks and will only download content I’m legally entitled to access and use.",
          )}
        </span>
      </button>
      {settings.acknowledged && (
        <p className="fine">
          {t(
            "Turning off a source hides its download options and pauses its unfinished downloads. Saved files stay on your drive. Re-enable the source and resume from Downloads when you’re ready.",
          )}
        </p>
      )}
      {error && (
        <p className="form-error" role="alert">
          {error}
        </p>
      )}
      <div className="dialog-actions">
        <button
          className="primary"
          disabled={
            pending ||
            !acknowledged ||
            (!settings.acknowledged && !selected.length)
          }
        >
          {pending
            ? t("Saving…")
            : !paired
              ? t("Pair to save sources")
              : t("Save sources")}
        </button>
      </div>
      <p className="fine">
        {t("Saved for this console and its paired devices.")}
      </p>
      <CommunityCatalogues
        paired={paired}
        pair={pair}
        canRefresh={settings.acknowledged}
      />
    </form>
  );
}

const noticeKey = "orbit.community-catalogue-notice.v1";

function sourceError(source: RemoteSource) {
  switch (source.lastError) {
    case "": return "";
    case "invalid-entries":
      return t("Skipped catalogue entries: {count}", { count: formatNumber(source.skippedEntries) });
    case "invalid-catalogue": return t("The response is not a Pegasus catalogue.");
    case "too-large": return t("The catalogue exceeds 8 MB.");
    case "storage": return t("Could not save community catalogues.");
    case "limit": return t("The catalogue contains more options than Orbit can store.");
    default: return t("Could not refresh this catalogue.");
  }
}

function CommunityCatalogues({ paired, pair, canRefresh }: {
  paired: boolean;
  pair: () => void;
  canRefresh: boolean;
}) {
  const [snapshot, setSnapshot] = useState<RemoteSources | null>(null);
  const [url, setUrl] = useState("");
  const [notice, setNotice] = useState(false);
  const [noticeRead, setNoticeRead] = useState(() => {
    try { return localStorage.getItem(noticeKey) === "1"; }
    catch { return false; }
  });
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const requestVersion = useRef(0);
  const [refreshAfter, setRefreshAfter] = useState(0);
  const retryAfter = Math.ceil(Math.max(0, refreshAfter - Date.now()) / 1000);

  const update = useCallback((data: RemoteSources) => {
    setSnapshot(data);
    setRefreshAfter(data.retryAfter ? Date.now() + data.retryAfter * 1000 : 0);
  }, []);
  useEffect(() => {
    if (!refreshAfter) return;
    const timer = window.setTimeout(() => setRefreshAfter(0), Math.max(0, refreshAfter - Date.now()));
    return () => window.clearTimeout(timer);
  }, [refreshAfter]);

  useEffect(() => {
    if (!paired || busy) return;
    let alive = true;
    async function poll() {
      const version = requestVersion.current;
      try {
        const data = await remoteSources();
        if (alive && version === requestVersion.current) update(data);
      } catch (e) {
        if (alive && version === requestVersion.current)
          setError((current) => current || (e as Error).message);
      }
    }
    void poll();
    const timer = window.setInterval(poll, 3000);
    return () => { alive = false; window.clearInterval(timer); };
  }, [paired, busy, update]);

  async function change(run: () => Promise<RemoteSources>, done?: () => void) {
    requestVersion.current++;
    setBusy(true);
    setError("");
    try {
      update(await run());
      done?.();
    } catch (e) {
      if (e instanceof ApiError && e.status === 429 && e.retryAfter)
        setRefreshAfter(Date.now() + e.retryAfter * 1000);
      else setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  }

  function add() {
    if (!url.trim()) return;
    if (!noticeRead && !notice) {
      setNotice(true);
      return;
    }
    void change(() => addRemoteSource(url.trim()), () => {
      setUrl("");
      setNotice(false);
      setNoticeRead(true);
      try { localStorage.setItem(noticeKey, "1"); } catch { /* Session-only preference. */ }
    });
  }

  return (
    <section className="community-catalogues" aria-label={t("Community catalogues")}>
      <h2>{t("Community catalogues")}</h2>
      {!paired ? <button type="button" onClick={pair}>{t("Pair to manage catalogues")}</button> : <>
        {snapshot?.sources.map((source) => (
          <div key={source.id} className="community-source">
            <button
              type="button"
              className="source-row"
              role="switch"
              aria-checked={source.enabled}
              disabled={busy}
              onClick={() => void change(() => enableRemoteSource(source.id, !source.enabled))}
            >
              <span className="source-row-text">
                <strong>{source.name === "Custom catalogue" ? t("Custom catalogue") : source.name}</strong>
                <small>
                  {tn(source.entryCount, "{count} download option", "{count} download options",
                    { count: formatNumber(source.entryCount) })}
                  {" · "}
                  {source.lastFetchedAt
                    ? formatDate(source.lastFetchedAt * 1000, { dateStyle: "short", timeStyle: "short" })
                    : t("Never refreshed")}
                </small>
                {source.lastError && <small>{sourceError(source)}</small>}
              </span>
              <span className="source-switch" aria-hidden="true" />
            </button>
            {!source.builtIn && (
              <button type="button" disabled={busy} onClick={() => void change(() => removeRemoteSource(source.id))}>
                {t("Remove")}
              </button>
            )}
          </div>
        ))}
        <div className="community-add">
          <input
            type="text"
            inputMode="url"
            maxLength={2048}
            aria-label={t("Add catalogue URL")}
            placeholder={t("Add catalogue URL")}
            value={url}
            disabled={busy}
            onChange={(event) => { setUrl(event.target.value); setNotice(false); }}
            onKeyDown={(event) => { if (event.key === "Enter") { event.preventDefault(); add(); } }}
          />
          <button type="button" disabled={busy || !url.trim()} onClick={add}>
            {notice ? t("Acknowledge and add") : t("Add catalogue")}
          </button>
        </div>
        {notice && <p className="fine" role="status">
          {t("This is a third-party catalogue. Orbit does not vet it. You are responsible for having permission to download the files it lists.")}
        </p>}
        <button
          type="button"
          disabled={busy || !canRefresh || !snapshot?.sources.some((source) => source.enabled && !source.bundled) || retryAfter > 0}
          onClick={() => void change(refreshRemoteSources)}
        >
          {t("Refresh now")}
        </button>
        {snapshot?.refreshing && <p className="fine" role="status">{t("Refreshing catalogues…")}</p>}
        {retryAfter > 0 && (
          <p className="fine">{t("Please wait {seconds} s before another request.", { seconds: retryAfter })}</p>
        )}
        {error && <p className="form-error" role="alert">{error}</p>}
      </>}
    </section>
  );
}
