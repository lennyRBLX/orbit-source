import { Select } from "./Select";
import { useEffect, useRef, useState } from "react";
import { api } from "./api";
import { Icon, Modal } from "./components";
import { bytes, type Job } from "./types";
import { t } from "./i18n";
export type HistoryChoice = "ask" | "keep" | "delete";
type Plan = {
  preference: HistoryChoice;
  downloads: {
    id: string;
    title: string;
    deleteAllowed: boolean;
    error: string;
    files: { path: string; sizeBytes: number }[];
  }[];
};

export function HistoryRemoval({
  jobs,
  close,
  done,
}: {
  jobs: Job[];
  close: () => void;
  done: (message: string) => Promise<void>;
}) {
  const [plan, setPlan] = useState<Plan | null>(null),
    [error, setError] = useState(""),
    [busy, setBusy] = useState(false),
    [remember, setRemember] = useState(false);
  const active = useRef(false);
  const ids = jobs.map((j) => j.id),
    key = JSON.stringify(ids);
  async function remove(
    choice: Exclude<HistoryChoice, "ask">,
    save = remember,
  ) {
    if (active.current) return;
    active.current = true;
    setBusy(true);
    setError("");
    try {
      await api("/history", { action: "remove", ids, choice, remember: save });
      const paths = jobs
        .map((j) => j.path)
        .filter(Boolean)
        .join("\n");
      await done(
        choice === "delete"
          ? t(
              "Deleting downloaded files. History is removed only after deletion succeeds.",
            )
          : `${t("History removed. Downloaded files were kept.")}${paths ? `\n${paths}` : ""}`,
      );
    } catch (e) {
      setError((e as Error).message);
      active.current = false;
    } finally {
      setBusy(false);
    }
  }
  useEffect(() => {
    let live = true;
    void api<Plan>("/history", { action: "preview", ids: JSON.parse(key) })
      .then((value) => {
        if (!live) return;
        setPlan(value);
        if (
          value.preference === "keep" ||
          (value.preference === "delete" &&
            value.downloads.every((d) => d.deleteAllowed))
        )
          void remove(value.preference, false);
      })
      .catch((e) => {
        if (live) setError(e.message);
      });
    return () => {
      live = false;
    };
  }, [key]);
  const allowed = plan?.downloads.every((d) => d.deleteAllowed);
  const total =
    plan?.downloads.reduce(
      (n, d) => n + d.files.reduce((s, f) => s + f.sizeBytes, 0),
      0,
    ) || 0;
  return (
    <Modal
      title={t("Remove from history?")}
      close={() => {
        if (!busy) close();
      }}
    >
      <h2>{t("Remove from history?")}</h2>
      <p>
        {t(
          "Choose whether to keep the downloaded files or delete them too. Installed packages are not uninstalled.",
        )}
      </p>
      {plan ? (
        <>
          <p>
            {t("Downloaded files")} · {bytes(total)}
          </p>
          <ul className="history-files">
            {plan.downloads.map((d) => (
              <li key={d.id}>
                <strong>{d.title}</strong>
                {d.files.map((f) => (
                  <p key={f.path}>
                    {f.path} · {bytes(f.sizeBytes)}
                  </p>
                ))}
                {d.error && <p role="alert">{t(d.error)}</p>}
              </li>
            ))}
          </ul>
        </>
      ) : (
        !error && <p>{t("Checking downloaded files…")}</p>
      )}
      <button
        type="button"
        role="checkbox"
        aria-checked={remember}
        className="source-acknowledgement"
        disabled={busy}
        onClick={() => setRemember(!remember)}
      >
        <span className="choice-check" aria-hidden="true">
          {remember && <Icon name="check" />}
        </span>
        <span>{t("Remember my choice")}</span>
      </button>
      <p>{t("You can change this later in Storage & cleanup.")}</p>
      {error && <p role="alert">{t(error)}</p>}
      <div className="dialog-actions">
        <button autoFocus disabled={busy} onClick={() => void remove("keep")}>
          {t("Keep files")}
        </button>
        <button
          className="danger"
          disabled={busy || !allowed}
          onClick={() => void remove("delete")}
        >
          {t("Delete files")}
        </button>
        <button disabled={busy} onClick={close}>
          {t("Go back")}
        </button>
      </div>
    </Modal>
  );
}

export function HistorySetting({
  value,
  refresh,
}: {
  value: HistoryChoice;
  refresh: () => Promise<unknown>;
}) {
  const [busy, setBusy] = useState(false),
    [error, setError] = useState("");
  async function save(choice: string) {
    setBusy(true);
    setError("");
    try {
      await api("/history", { action: "settings", choice });
      await refresh();
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  }
  return (
    <section className="archive-setting">
      <h2 className="download-storage-heading">
        {t("Removing download history")}
      </h2>
      <Select
        label={t("Downloaded files")}
        disabled={busy}
        value={value}
        onChange={(choice) => void save(choice)}
        options={[
          { value: "ask", label: t("Ask every time") },
          { value: "keep", label: t("Keep files") },
          { value: "delete", label: t("Delete files") },
        ]}
      />
      {error && <p role="alert">{t(error)}</p>}
    </section>
  );
}
