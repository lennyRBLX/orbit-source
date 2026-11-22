import { Select } from "./Select";
import { useRef, useState } from "react";
import { api } from "./api";
import { Icon, Modal } from "./components";
import { t } from "./i18n";
import type { Job, System } from "./types";

export function PowerSetting({
  system,
  jobs,
  refresh,
  countdownOnly = false,
}: {
  system: System | null;
  jobs: Job[];
  refresh: () => Promise<unknown>;
  countdownOnly?: boolean;
}) {
  const [busy, setBusy] = useState(false),
    [error, setError] = useState("");
  const power = system?.power;
  if (!power?.available || (countdownOnly && power.state !== "countdown"))
    return null;
  async function change(enabled: boolean, jobId = "") {
    if (busy) return;
    setBusy(true);
    setError("");
    try {
      await api("/power", { enabled, jobId });
      await refresh();
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  }
  if (countdownOnly)
    return (
      <Modal
        title={t("Automatic PS5 shutdown")}
        close={() => {
          void change(false);
        }}
      >
        <h2>{t("Automatic PS5 shutdown")}</h2>
        <p role="status">
          {t("This PS5 will turn off in {seconds} seconds.", {
            seconds: String(power.seconds),
          })}
        </p>
        <button autoFocus disabled={busy} onClick={() => void change(false)}>
          {t("Cancel automatic shutdown")}
        </button>
        {error && <p role="alert">{t(error)}</p>}
      </Modal>
    );
  return (
    <section className="archive-setting">
      <h2 className="download-storage-heading">
        {t("Automatic PS5 shutdown")}
      </h2>
      {power.state === "countdown" ? (
        <p>
          {t("This PS5 will turn off in {seconds} seconds.", {
            seconds: String(power.seconds),
          })}
        </p>
      ) : (
        !countdownOnly && (
          <p>
            {t(
              "Turn off the PS5 after downloads, transfers and installations finish. A 60-second countdown lets you cancel. Keep Orbit Zero open while it has unfinished downloads.",
            )}
          </p>
        )
      )}
      {!countdownOnly && (
        <button
          type="button"
          role="checkbox"
          aria-checked={power.enabled}
          className="source-acknowledgement"
          disabled={busy}
          onClick={() => void change(!power.enabled)}
        >
          <span className="choice-check" aria-hidden="true">
            {power.enabled && <Icon name="check" />}
          </span>
          <span>{t("Shut down when the queue finishes")}</span>
        </button>
      )}
      {!countdownOnly && power.enabled && (
        <Select
          label={t("Wait for")}
          value={power.jobId}
          disabled={busy}
          onChange={(value) => void change(true, value)}
          options={[
            { value: "", label: t("All downloads") },
            ...jobs
              .filter(
                (j) => j.status !== "cancelled" && !j.id.startsWith("zero-"),
              )
              .map((j) => ({ value: j.id, label: j.title || j.filename })),
          ]}
        />
      )}

      {power.enabled && (
        <button disabled={busy} onClick={() => void change(false)}>
          {t("Cancel automatic shutdown")}
        </button>
      )}
      {(error || power.error) && <p role="alert">{t(error || power.error)}</p>}
    </section>
  );
}

export function PowerJobDialog({ job, enabled, close, done, refresh }: {
  job: Job; enabled: boolean; close: () => void;
  done: (message: string) => void; refresh: () => Promise<unknown>;
}) {
  const active = useRef(false);
  const [busy, setBusy] = useState(false), [error, setError] = useState("");
  async function confirm() {
    if (active.current) return;
    active.current = true; setBusy(true); setError("");
    try {
      await api("/power", { enabled, jobId: enabled ? job.id : "" });
      // The mutation succeeded even if the following status refresh is interrupted.
      await refresh().catch(() => {});
      done(enabled ? t("Automatic shutdown scheduled.") : t("Automatic shutdown cancelled."));
    } catch (e) { setError((e as Error).message); }
    finally { active.current = false; setBusy(false); }
  }
  return <Modal title={t("Automatic PS5 shutdown")} close={() => { if (!active.current) close(); }}>
    <h2>{t(enabled ? "Shut down when finished" : "Cancel automatic shutdown")}</h2>
    <p><bdi>{job.title || job.filename}</bdi></p>
    {enabled && <p>{t("Turn off the PS5 after downloads, transfers and installations finish. A 60-second countdown lets you cancel. Keep Orbit Zero open while it has unfinished downloads.")}</p>}
    {error && <p role="alert">{t(error)}</p>}
    <div className="dialog-actions">
      <button autoFocus data-initial-focus disabled={busy} onClick={close}>{t("Go back")}</button>
      <button className="primary" disabled={busy} onClick={() => void confirm()}>{t(enabled ? "Shut down when finished" : "Cancel automatic shutdown")}</button>
    </div>
  </Modal>;
}
