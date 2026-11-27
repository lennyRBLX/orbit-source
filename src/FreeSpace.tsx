// Free up space (POST /api/v1/space). Opens when a download doesn't fit on a drive,
// when a download pauses without room to unpack, and from Library. It lists what
// Orbit can delete on one drive, biggest first: its own leftovers (ticked) and the
// games ShadowMount can delete (never ticked). The PS5 deletes the chosen items one
// at a time, then the download starts or the unpack resumes by itself.
import { useEffect, useRef, useState, useSyncExternalStore } from "react";
import { api } from "./api";
import { Icon, Modal } from "./components";
import { Select } from "./Select";
import { bytes, type Storage } from "./types";
import { placeName, t, tn } from "./i18n";

export interface SpaceItem {
  id: string;
  kind: "package" | "archive" | "partial" | "game";
  title: string;
  name?: string;
  sizeBytes: number;
  format?: string;
  mounted?: boolean;
}
export interface SpaceStatus {
  state: "idle" | "deleting" | "done" | "failed";
  storageId: string;
  message: string;
  current: string;
  total: number;
  done: number;
  failed: number;
  freedBytes: number;
}
export interface SpaceList {
  storageId: string;
  label: string;
  freeBytes: number;
  totalBytes: number;
  // Free space minus what unfinished downloads still need, as Download counts it.
  projectedFreeBytes: number;
  leftovers: SpaceItem[];
  games: SpaceItem[];
  deletion: SpaceStatus;
}
export interface SpaceRequest {
  storageId: string;
  // What didn't fit: its size, and the projected free space it needs on this drive.
  need?: { sizeBytes: number; bytes: number };
  // Runs once there's room: the download starts, or the unpack resumes.
  then?: { kind: "download" | "resume"; run: () => void };
  // Other drives where it fits, offered on the Not enough space sheet.
  others?: Storage[];
  useDrive?: (id: string) => void;
}

// One screen at a time, opened from anywhere: the game page, Library or Downloads.
let open: { request: SpaceRequest; serial: number } | null = null;
let serial = 0;
const listeners = new Set<() => void>();
function publish(next: typeof open) {
  open = next;
  listeners.forEach((listener) => listener());
}
export function openFreeSpace(request: SpaceRequest) {
  publish({ request, serial: ++serial });
}
function closeFreeSpace() {
  publish(null);
}

// How much more room is needed, after what's free now.
export function spaceShort(need: number, projectedFree: number) {
  return Math.max(0, need - projectedFree);
}

// Each item's selected size, and the leftovers Orbit ticks for the user.
export function selectedBytes(list: SpaceList | null, selected: Set<string>) {
  return [...(list?.leftovers || []), ...(list?.games || [])]
    .filter((item) => selected.has(item.id))
    .reduce((sum, item) => sum + item.sizeBytes, 0);
}
export function defaultSelection(list: SpaceList) {
  return new Set(list.leftovers.map((item) => item.id));
}

const kindLabels: Record<Exclude<SpaceItem["kind"], "game">, string> = {
  package: /* i18n */ "Installed package",
  archive: /* i18n */ "Archive kept after unpacking",
  partial: /* i18n */ "Cancelled download",
};

export function FreeSpaceHost({ drives, refresh }: { drives: Storage[]; refresh: () => Promise<unknown> }) {
  const current = useSyncExternalStore(
    (listener) => {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
    () => open,
  );
  if (!current) return null;
  return <FreeSpace key={current.serial} request={current.request} drives={drives} refresh={refresh} close={closeFreeSpace} />;
}

function FreeSpace({
  request,
  drives,
  refresh,
  close,
}: {
  request: SpaceRequest;
  drives: Storage[];
  refresh: () => Promise<unknown>;
  close: () => void;
}) {
  const [storageId, setStorageId] = useState(request.storageId);
  const [step, setStep] = useState<"short" | "list" | "confirm" | "deleting">(request.need ? "short" : "list");
  const [list, setList] = useState<SpaceList | null>(null);
  const [selected, setSelected] = useState<Set<string>>(new Set());
  const [status, setStatus] = useState<SpaceStatus | null>(null);
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");
  const [busy, setBusy] = useState(false);
  const alive = useRef(true);
  useEffect(() => () => { alive.current = false; }, []);
  const drive = drives.find((d) => d.id === storageId);
  const free = list?.projectedFreeBytes ?? drive?.projectedFreeBytes ?? 0;
  const short = request.need ? spaceShort(request.need.bytes, free) : 0;
  const chosen = selectedBytes(list, selected);
  const items = [...(list?.leftovers || []), ...(list?.games || [])];
  const chosenItems = items.filter((item) => selected.has(item.id));

  async function load(id: string, keep = false) {
    setError("");
    try {
      const next = await api<SpaceList>("/space", { action: "list", storageId: id });
      if (!alive.current) return null;
      setList(next);
      setSelected((previous) =>
        keep
          ? new Set([...previous].filter((x) => [...next.leftovers, ...next.games].some((item) => item.id === x)))
          : defaultSelection(next),
      );
      // Another device started deleting on this drive: follow it here too.
      if (next.deletion.state === "deleting" && next.deletion.storageId === id) {
        setStatus(next.deletion);
        setStep("deleting");
        void follow();
      }
      return next;
    } catch (e) {
      if (alive.current) setError((e as Error).message);
      return null;
    }
  }
  useEffect(() => {
    if (step === "list" && !list) void load(storageId);
    // Only when the list is first needed or the drive changes.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [step, storageId]);

  async function finish(done: SpaceStatus) {
    void refresh().catch(() => {});
    const next = await load(storageId);
    if (!alive.current) return;
    setStep("list");
    if (done.state === "failed") {
      setError(done.message ? t(done.message) : t("An item couldn't be deleted. It was kept."));
      return;
    }
    if (!request.then) {
      setNotice(t("Deleted {size}.", { size: bytes(done.freedBytes) }));
      return;
    }
    const left = request.need && next ? spaceShort(request.need.bytes, next.projectedFreeBytes) : 0;
    if (left > 0) {
      setError(t("Still {size} short. Select more to delete.", { size: bytes(left) }));
      return;
    }
    close();
    request.then.run();
  }
  async function follow() {
    for (;;) {
      await new Promise((resolve) => setTimeout(resolve, 600));
      if (!alive.current) return;
      const next = await api<SpaceStatus>("/space").catch(() => null);
      if (!alive.current) return;
      if (!next) continue;
      setStatus(next);
      if (next.state !== "deleting") {
        await finish(next);
        return;
      }
    }
  }
  async function remove() {
    setBusy(true);
    setError("");
    setNotice("");
    try {
      const started = await api<SpaceStatus>("/space", { action: "delete", storageId, ids: chosenItems.map((item) => item.id) });
      if (!alive.current) return;
      setStatus(started);
      setStep("deleting");
      void follow();
    } catch (e) {
      if (!alive.current) return;
      setError((e as Error).message);
      setStep("list");
      void load(storageId, true);
    } finally {
      if (alive.current) setBusy(false);
    }
  }
  function toggle(id: string) {
    setNotice("");
    setSelected((previous) => {
      const next = new Set(previous);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return next;
    });
  }

  const actionLabel = request.then?.kind === "resume" ? t("Delete and resume") : request.then ? t("Delete and download") : t("Delete");
  const title = step === "short" ? t("Not enough space") : t("Free up space");
  const driveName = placeName(list?.label || drive?.label || "");
  const row = (item: SpaceItem) => (
    <li key={item.id}>
      <button type="button" className="space-item" aria-pressed={selected.has(item.id)} onClick={() => toggle(item.id)}>
        <span className="space-check" aria-hidden="true">{selected.has(item.id) && <Icon name="check" />}</span>
        <span className="space-item-text">
          <strong dir="auto">{item.title || item.name}</strong>
          <small dir="auto">
            {item.kind === "game"
              ? [item.format, item.mounted ? t("Mounted") : ""].filter(Boolean).join(" · ")
              : [t(kindLabels[item.kind]), item.name].filter(Boolean).join(" · ")}
          </small>
        </span>
        <span className="space-item-size">{bytes(item.sizeBytes)}</span>
      </button>
    </li>
  );

  return (
    <Modal title={title} className="free-space" close={close}>
      <div className="archive-password-form free-space-form">
        <h2>{title}</h2>
        {step === "short" && request.need && (
          <>
            <p className="space-summary">
              {t("Needs {size} · {free} free on {drive} · {missing} more needed", {
                size: bytes(request.need.sizeBytes),
                free: bytes(Math.max(0, free)),
                drive: driveName,
                missing: bytes(short),
              })}
            </p>
            {!!drive?.pendingBytes && <p className="fine">{t("Unfinished downloads keep the room they still need.")}</p>}
            <div className="actions">
              <button className="primary" data-initial-focus onClick={() => setStep("list")}>
                {t("Free up space")}
              </button>
              {(request.others || []).slice(0, 2).map((other) => (
                <button key={other.id} onClick={() => { close(); request.useDrive?.(other.id); }}>
                  {t("Use {drive} ({free} free)", { drive: placeName(other.label), free: bytes(other.projectedFreeBytes) })}
                </button>
              ))}
              <button onClick={close}>{t("Cancel")}</button>
            </div>
          </>
        )}
        {step === "list" && (
          <>
            {!request.need && drives.length > 1 && (
              <Select
                label={t("Drive")}
                value={storageId}
                onChange={(id) => { setStorageId(id); setList(null); setNotice(""); }}
                options={drives.map((d) => ({
                  value: d.id,
                  label: `${placeName(d.label)} · ${t("{size} free", { size: bytes(d.freeBytes) })}`,
                }))}
              />
            )}
            {request.need ? (
              <div className={`space-meter${chosen >= short ? " met" : ""}`} role="status">
                <span>{t("{selected} selected of {missing} needed", { selected: bytes(chosen), missing: bytes(short) })}</span>
                <span className="space-meter-bar" aria-hidden="true">
                  <span style={{ width: `${short ? Math.min(100, (chosen / short) * 100) : 100}%` }} />
                </span>
              </div>
            ) : (
              list && (
                <p className="space-summary" role="status">
                  {t("{free} free of {total}", { free: bytes(list.freeBytes), total: bytes(list.totalBytes) })}
                  {" · "}
                  {t("{size} selected", { size: bytes(chosen) })}
                </p>
              )
            )}
            {!list && !error && <p className="fine">{t("Checking this drive…")}</p>}
            {list && !items.length && <p className="fine">{t("Orbit has nothing it can delete on this drive.")}</p>}
            {!!list?.leftovers.length && (
              <section className="space-group" aria-label={t("Left by Orbit")}>
                <h3>{t("Left by Orbit")}</h3>
                <ul className="space-items">{list.leftovers.map(row)}</ul>
              </section>
            )}
            {!!list?.games.length && (
              <section className="space-group" aria-label={t("Games")}>
                <h3>{t("Games")}</h3>
                <ul className="space-items">{list.games.map(row)}</ul>
              </section>
            )}
            {notice && <p className="space-notice" role="status">{notice}</p>}
            {error && <p className="job-error" role="alert">{error}</p>}
            <div className="actions">
              <button
                className="primary"
                data-initial-focus
                disabled={!chosenItems.length || (!!request.need && chosen < short)}
                onClick={() => { setError(""); setStep("confirm"); }}
              >
                {actionLabel}
              </button>
              <button onClick={close}>{t("Cancel")}</button>
            </div>
          </>
        )}
        {step === "confirm" && (
          <>
            <p>
              {tn(chosenItems.length, "Delete {count} item, {size}?", "Delete {count} items, {size}?", { size: bytes(chosen) })}{" "}
              {t("Saves are not touched.")}
            </p>
            <ul className="space-items space-confirm">
              {chosenItems.map((item) => (
                <li key={item.id}>
                  <span className="space-item">
                    <span className="space-item-text">
                      <strong dir="auto">{item.title || item.name}</strong>
                    </span>
                    <span className="space-item-size">{bytes(item.sizeBytes)}</span>
                  </span>
                </li>
              ))}
            </ul>
            {error && <p className="job-error" role="alert">{error}</p>}
            <div className="actions">
              <button className="primary danger" data-initial-focus disabled={busy} onClick={() => void remove()}>
                {actionLabel}
              </button>
              <button data-dialog-back disabled={busy} onClick={() => setStep("list")}>
                {t("Back")}
              </button>
            </div>
          </>
        )}
        {step === "deleting" && (
          <>
            <p role="status">
              {status?.current ? t("Deleting {title}…", { title: status.current }) : t("Deleting…")}
            </p>
            {!!status?.total && (
              <div className="space-meter" aria-hidden="true">
                <span>{t("{done} of {total}", { done: status.done, total: status.total })}</span>
                <span className="space-meter-bar">
                  <span style={{ width: `${(status.done / status.total) * 100}%` }} />
                </span>
              </div>
            )}
            <div className="actions">
              <button data-initial-focus onClick={close}>{t("Close")}</button>
            </div>
          </>
        )}
      </div>
    </Modal>
  );
}
