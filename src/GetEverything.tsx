import { releaseProvider } from "./types";
// Get everything: the game and its DLC in one checklist, added to Downloads in
// order with Download all (POST /api/v1/bundles), then each row's progress. One
// model; each look draws it its own way.
import { useEffect, useMemo, useRef, useState, type KeyboardEvent, type RefObject } from "react";
import {
  bytes,
  type Bundle,
  type BundleItem,
  type Game,
  type Job,
  type LibraryGame,
  type Release,
  type Storage,
  type System,
} from "./types";
import {
  addedCount,
  bundleFor,
  bundleRequest,
  bundleRunning,
  checklistRows,
  chosenRows,
  defaultTicks,
  hasExtras,
  rowProgress,
  spaceFor,
  type ChecklistRow,
  type RowProgress,
} from "./bundle";
import { preferredDelivery, type DebridStatus } from "./Debrid";
import { rememberChoice } from "./downloadChoice";
import { placeName, t } from "./i18n";
import { Icon } from "./components";
import { Glyph } from "./interfaces/Glyph";
import { PnGroup, PnTile } from "./interfaces/PanoramaParts";
import { menuKeys } from "./interfaces/menuKeys";

// What App hands every game page for Get everything.
export interface BundleCatalog {
  addons: Game[];
  bundle: Bundle | null;
  startBundle: (body: Record<string, unknown>) => Promise<Bundle>;
  cancelBundle: (id: string) => Promise<Bundle>;
  watchBundle: () => () => void;
}

export const rowTitle = (row: ChecklistRow) => (row.kind === "game" ? t("Game") : t("{title} (DLC)", { title: row.title }));

// Cinema's and Panorama's small label over a card: what kind of item it is.
export const rowKind = (row: ChecklistRow) => (row.kind === "game" ? t("Game") : "DLC");

// The source.
export const rowSource = (row: ChecklistRow) => releaseProvider(row.release);

export const progressLabel = (p: RowProgress) =>
  p === "added" ? t("Added") : p === "page" ? t("Open its page on your PS5") : p === "not-added" ? t("Not added") : t("Waiting");

// "Game and 2 DLC": what Get everything offers, before the size.
export function offerText(rows: ChecklistRow[]) {
  const game = rows.find((r) => r.kind === "game");
  const count = rows.filter((r) => r.kind === "dlc").length;
  if (game && !game.installed) return count ? t("Game and {count} DLC", { count }) : t("Game");
  return t("{count} DLC", { count });
}

export function useEverything({
  game,
  release,
  delivery,
  drive,
  jobs,
  library,
  system,
  debrid,
  catalog,
}: {
  game: Game;
  release: Release;
  delivery: "direct" | "torbox";
  drive: Storage | undefined;
  jobs: Job[];
  library: LibraryGame[];
  system: System | null;
  debrid: DebridStatus | null;
  catalog: BundleCatalog;
}) {
  const rows = useMemo(
    () =>
      game.addonId ? [] : checklistRows({ release, addons: catalog.addons, library }),
    [game.addonId, release, catalog.addons, library],
  );
  const basis = rows.map((r) => `${r.key}:${r.release.id}:${r.installed}`).join("\n");
  const [ticks, setTicks] = useState<{ basis: string; keys: Set<string> } | null>(null);
  const ticked = ticks?.basis === basis ? ticks.keys : defaultTicks(rows);
  const chosen = chosenRows(rows, ticked);
  const space = spaceFor(chosen.map((r) => r.release), drive, jobs);
  const offered = chosenRows(rows, defaultTicks(rows));
  const [pending, setPending] = useState(false);
  const [error, setError] = useState("");
  const [startedId, setStartedId] = useState("");
  const progress = bundleFor(catalog.bundle, rows.map((r) => r.release.id), startedId);
  const running = bundleRunning(progress);
  const items = progress
    ? progress.items.map((item) => ({ item, row: rows.find((r) => r.release.id === item.releaseId), progress: rowProgress(item) }))
    : [];
  const pageItem = items.find((i) => i.progress === "page");
  function toggle(key: string) {
    const row = rows.find((r) => r.key === key);
    if (!row || (row.kind === "game" && row.installed) || progress) return;
    const next = new Set(ticked);
    if (next.has(key)) next.delete(key);
    else next.add(key);
    setTicks({ basis, keys: next });
  }
  async function start() {
    if (!drive || !chosen.length) return;
    setPending(true);
    setError("");
    try {
      if (chosen.some((r) => r.kind === "game")) rememberChoice(release);
      const status = await catalog.startBundle(
        bundleRequest(
          drive.id,
          chosen.map((r) => ({
            release: r.release,
            delivery: r.kind === "game" && r.release.id === release.id ? delivery : preferredDelivery(debrid, r.release),
          })),
          true,
        ),
      );
      setStartedId(status.id);
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setPending(false);
    }
  }
  async function stop() {
    if (!progress) return;
    setPending(true);
    setError("");
    try {
      await catalog.cancelBundle(progress.id);
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setPending(false);
    }
  }
  // Closing leaves a running bundle going; a finished one isn't shown again.
  function dismiss() {
    setStartedId("");
    setError("");
  }
  const watchBundle = catalog.watchBundle;
  // While a page shows this checklist, its bundle's progress stays fresh.
  function useWatch(open: boolean) {
    useEffect(() => (open ? watchBundle() : undefined), [open]);
  }
  return {
    game,
    release,
    rows,
    available: !!system?.paired && system.bundleProtocol === 1 && hasExtras(rows),
    ticked,
    chosen,
    toggle,
    total: space.total,
    free: space.free,
    enough: space.enough,
    drive,
    offer: `${offerText(rows)} · ${bytes(offered.reduce((n, r) => n + r.release.sizeBytes, 0))}`,
    canStart: !!drive && chosen.length > 0 && space.enough && !pending && !bundleRunning(catalog.bundle),
    // Why Download all can't start, for the line above it.
    blocker: !drive
      ? t("Connect a writable drive to your PS5.")
      : chosen.length && !space.enough
        ? t("Not enough space on this drive.")
        : !progress && bundleRunning(catalog.bundle)
          ? t("Wait for the current Download all to finish.")
          : "",
    pending,
    error: error || (progress && !running && progress.message ? t(progress.message) : ""),
    progress,
    running,
    items,
    pageItem,
    start,
    stop,
    dismiss,
    useWatch,
  };
}
export type Everything = ReturnType<typeof useEverything>;

export const totalLine = (m: Everything) => t("Total {total} · Free {free}", { total: bytes(m.total), free: bytes(m.free) });
export const orderLine = (m: Everything) =>
  [m.drive ? t("To {drive}.", { drive: placeName(m.drive.label) }) : "", t("Installs in order: game, then DLC.")].filter(Boolean).join(" ");
export const addingLine = (m: Everything) =>
  m.progress ? t("Adding {done} of {total} to Downloads", { done: addedCount(m.progress), total: m.progress.items.length }) : "";
const itemTitle = (entry: { item: BundleItem; row?: ChecklistRow }) => (entry.row ? rowTitle(entry.row) : entry.item.title);
// Its source and size, and a failed item's own reason.
const itemSub = (entry: { item: BundleItem; row?: ChecklistRow }) =>
  [
    entry.row ? rowSource(entry.row) : "",
    entry.row ? bytes(entry.row.release.sizeBytes) : "",
    entry.item.state === "failed" && entry.item.message ? t(entry.item.message) : "",
  ]
    .filter(Boolean)
    .join(" · ");

// When Download all starts (or the panel opens on one), the focus goes to its
// buttons (Stop, or Close), since the button pressed is gone.
function useProgressFocus(m: Everything, box: RefObject<HTMLElement | null>, buttons: string) {
  const id = m.progress?.id || "";
  useEffect(() => {
    if (!id) return;
    requestAnimationFrame(() => box.current?.querySelector<HTMLElement>(`${buttons} button:not([disabled])`)?.focus({ preventScroll: true }));
  }, [id]);
}

// What to do on a provider's page, as a single Download explains it.
function pageSteps(release: Release | undefined) {
  if (!release || release.sourceId === "akirabox" || release.sourceId === "vikingfile")
    return t("Complete verification if shown, then wait. Orbit will check the generated download link automatically.");
  if (release.sourceId === "datanodes")
    return t("Scroll to the bottom, select Continue to Download, then Free Download. Wait for the countdown and select Start Download.");
  return t("On {provider}, complete any verification and select the site’s {button} button.", { provider: releaseProvider(release), button: t("Download") });
}

// The provider's page is open on the PS5 for the item in turn.
export function PageCard({ m, className = "everything-page" }: { m: Everything; className?: string }) {
  if (!m.pageItem) return null;
  return (
    <div className={className} role="status">
      <strong>{t("The {provider} page is open on your PS5", { provider: m.pageItem.row ? releaseProvider(m.pageItem.row.release) : m.pageItem.item.title })}</strong>
      <p>{pageSteps(m.pageItem.row?.release)} {t("Orbit then comes back and the next item starts.")}</p>
    </div>
  );
}

function StateMark({ p }: { p: RowProgress }) {
  return (
    <span className={`everything-mark is-${p}`} aria-hidden="true">
      {p === "added" && <Icon name="check" />}
    </span>
  );
}

// Store: a view of the game's download sheet. Left the rows, right the totals.
export function StoreEverything({ m, close }: { m: Everything; close: () => void }) {
  m.useWatch(true);
  const confirm = useRef<HTMLDivElement>(null);
  useProgressFocus(m, confirm, ".everything-buttons");
  return (
    <>
      <div className="sheet-scroll everything-scroll" data-scroll-region>
        <p className="everything-sub">
          {m.running ? addingLine(m) : [m.game.title, m.release.titleId, m.release.region].filter(Boolean).join(" · ")}
        </p>
        <PageCard m={m} />
        {m.progress ? (
          <div className="everything-rows" role="list">
            {m.items.map((entry) => (
              <div key={entry.item.releaseId} role="listitem" className={`sheet-choice everything-row is-${entry.progress}`}>
                <StateMark p={entry.progress} />
                <span>
                  <strong><bdi>{itemTitle(entry)}</bdi></strong>
                  <small>{itemSub(entry)}</small>
                </span>
                <span className="everything-state">{progressLabel(entry.progress)}</span>
              </div>
            ))}
          </div>
        ) : (
          <div className="everything-rows" role="group" aria-label={t("Get everything")}>
            {m.rows.map((row, i) => {
              const on = m.ticked.has(row.key) && !row.installed;
              return (
                <button
                  key={row.key}
                  className="sheet-choice everything-row"
                  aria-pressed={on}
                  disabled={row.installed || m.pending}
                  data-initial-focus={i === m.rows.findIndex((r) => !r.installed) || undefined}
                  onClick={() => m.toggle(row.key)}
                >
                  <span className="sheet-choice-check">{on && <Icon name="check" />}</span>
                  <span>
                    <strong><bdi>{rowTitle(row)}</bdi></strong>
                    <small>
                      {row.kind === "game" && row.release.version
                        ? `${releaseProvider(row.release)} · ${t("version {version}", { version: row.release.version })}`
                        : rowSource(row)}
                    </small>
                  </span>
                  <span className="everything-size">{row.installed ? t("Installed") : bytes(row.release.sizeBytes)}</span>
                </button>
              );
            })}
          </div>
        )}
      </div>
      <div className="sheet-confirm everything-confirm" ref={confirm}>
        {!m.progress && (
          <>
            <p className="everything-total">{totalLine(m)}</p>
            <p className="sheet-note">{orderLine(m)}</p>
            {m.blocker && <p className="sheet-note warn">{m.blocker}</p>}
          </>
        )}
        {m.error && <p className="form-error" role="alert">{m.error}</p>}
        <div className="everything-buttons">
          {m.progress ? (
            <>
              {m.running && (
                <button disabled={m.pending} onClick={() => void m.stop()}>
                  {t("Stop")}
                </button>
              )}
              <button className="primary sheet-primary" onClick={close}>
                {t("Close")}
              </button>
            </>
          ) : (
            <>
              <button onClick={close}>{t("Cancel")}</button>
              <button className="primary sheet-primary" disabled={!m.canStart} onClick={() => void m.start()}>
                {m.pending ? t("Please wait…") : t("Download all")}
              </button>
            </>
          )}
        </div>
      </div>
    </>
  );
}

// Crossbar: a glass card beside the rows, square ticks, white Download all.
export function CrossbarEverything({ m, close }: { m: Everything; close: () => void }) {
  m.useWatch(true);
  const card = useRef<HTMLDivElement>(null);
  useProgressFocus(m, card, ".xb-everything-buttons");
  return (
    <div
      ref={card}
      className="xb-menu xb-choice-menu xb-everything"
      role="dialog"
      aria-label={t("Get everything")}
      data-own-arrows
      onKeyDown={(e: KeyboardEvent<HTMLElement>) => menuKeys(e, close)}
    >
      <span className="xb-menu-title">{t("Get everything")}</span>
      <span className="xb-everything-sub">
        {m.running ? addingLine(m) : <bdi>{[m.game.title, m.release.titleId].join(" · ")}</bdi>}
      </span>
      <PageCard m={m} className="xb-everything-page" />
      {m.progress
        ? m.items.map((entry) => (
            <div key={entry.item.releaseId} role="listitem" className={`xb-everything-row is-${entry.progress}`}>
              <StateMark p={entry.progress} />
              <span>
                <strong><bdi>{itemTitle(entry)}</bdi></strong>
                {entry.row && <small> · {releaseProvider(entry.row.release)}</small>}
              </span>
              <span className="xb-everything-value">{progressLabel(entry.progress)}</span>
            </div>
          ))
        : m.rows.map((row, i) => {
            const on = m.ticked.has(row.key) && !row.installed;
            return (
              <button
                key={row.key}
                className="xb-everything-row"
                role="menuitemcheckbox"
                aria-checked={on}
                disabled={row.installed || m.pending}
                autoFocus={i === m.rows.findIndex((r) => !r.installed)}
                onClick={() => m.toggle(row.key)}
              >
                <span className="xb-check" aria-hidden="true">{on && <Glyph name="check" size={14} stroke={3} />}</span>
                <span>
                  <strong><bdi>{rowTitle(row)}</bdi></strong>
                  <small> · {releaseProvider(row.release)}</small>
                </span>
                <span className="xb-everything-value">{row.installed ? t("Installed") : bytes(row.release.sizeBytes)}</span>
              </button>
            );
          })}
      <div className="xb-everything-foot">
        {!m.progress && (
          <>
            <strong>{totalLine(m)}</strong>
            <span className="xb-everything-fine">{orderLine(m)}</span>
            {m.blocker && <span className="xb-everything-fine warn">{m.blocker}</span>}
          </>
        )}
        {m.error && <span className="xb-everything-fine warn" role="alert">{m.error}</span>}
      </div>
      <div className="xb-everything-buttons">
        {m.progress ? (
          <>
            {m.running && (
              <button className="xb-pill" disabled={m.pending} onClick={() => void m.stop()}>
                {t("Stop")}
              </button>
            )}
            <button className="xb-pill primary" onClick={close}>
              {t("Close")}
            </button>
          </>
        ) : (
          <>
            <button className="xb-pill" onClick={close}>
              {t("Cancel")}
            </button>
            <button className="xb-pill primary" disabled={!m.canStart} onClick={() => void m.start()}>
              {m.pending ? t("Please wait…") : `${t("Download all")} · ${bytes(m.total)}`}
            </button>
          </>
        )}
      </div>
    </div>
  );
}

// Cinema: the bottom sheet. Left the title, totals and buttons; right the cards.
export function CinemaEverything({ m, close }: { m: Everything; close: () => void }) {
  m.useWatch(true);
  const left = useRef<HTMLDivElement>(null);
  useProgressFocus(m, left, ".cn-actions");
  return (
    <>
      <div className="cn-sheet-left" ref={left}>
        <h2 className="cn-everything-title">{t("Get everything")}</h2>
        <PageCard m={m} className="cn-note cn-everything-page" />
        {m.progress ? (
          m.running && (
            <p className="cn-note">
              <strong className="cn-everything-total">{addingLine(m)}</strong>
            </p>
          )
        ) : (
          <p className="cn-note">
            <strong className="cn-everything-total">{totalLine(m)}</strong>
            {orderLine(m)}
          </p>
        )}
        {!m.progress && m.blocker && <p className="cn-note warn">{m.blocker}</p>}
        {m.error && (
          <p className="cn-note warn" role="alert">
            {m.error}
          </p>
        )}
        <div className="cn-actions">
          {m.progress ? (
            <>
              {m.running && (
                <button className="cn-button" disabled={m.pending} onClick={() => void m.stop()}>
                  {t("Stop")}
                </button>
              )}
              <button className="cn-button primary" onClick={close}>
                {t("Close")}
              </button>
            </>
          ) : (
            <>
              <button className="cn-button" onClick={close}>
                {t("Cancel")}
              </button>
              <button className="cn-button primary" disabled={!m.canStart} onClick={() => void m.start()}>
                {m.pending ? t("Please wait…") : t("Download all")}
              </button>
            </>
          )}
        </div>
      </div>
      <div className="cn-sheet-right">
        <p className="cn-section-label">{t("Included")}</p>
        <div className="cn-cards cn-choices cn-everything-cards">
          {m.progress
            ? m.items.map((entry) => (
                <div key={entry.item.releaseId} className={`cn-choice cn-everything-card is-${entry.progress}`}>
                  <small className="cn-everything-kind">{entry.row ? rowKind(entry.row) : entry.item.kind}</small>
                  <strong>
                    <bdi>{entry.row?.kind === "dlc" ? `${entry.row.title} · ${bytes(entry.row.release.sizeBytes)}` : entry.row ? bytes(entry.row.release.sizeBytes) : entry.item.title}</bdi>
                  </strong>
                  <small>{progressLabel(entry.progress)}</small>
                  {entry.progress === "added" && <Glyph name="check" size={16} stroke={2.4} />}
                </div>
              ))
            : m.rows.map((row) => {
                const on = m.ticked.has(row.key) && !row.installed;
                return (
                  <button
                    key={row.key}
                    className="cn-choice cn-everything-card"
                    aria-pressed={on}
                    aria-label={rowTitle(row)}
                    disabled={row.installed || m.pending}
                    onClick={() => m.toggle(row.key)}
                  >
                    <small className="cn-everything-kind">{rowKind(row)}</small>
                    <strong>
                      <bdi>{row.installed ? t("Installed") : row.kind === "dlc" ? `${row.title} · ${bytes(row.release.sizeBytes)}` : bytes(row.release.sizeBytes)}</bdi>
                    </strong>
                    <small>{rowSource(row)}</small>
                    {on && <Glyph name="check" size={16} stroke={2.4} />}
                  </button>
                );
              })}
        </div>
      </div>
    </>
  );
}

// Panorama: its own group of check tiles, Download all and the totals.
export function PanoramaEverything({ m }: { m: Everything }) {
  m.useWatch(!!m.progress);
  const group = useRef<HTMLDivElement>(null);
  useProgressFocus(m, group, ".pn-everything-buttons");
  return (
    <div ref={group} className="pn-everything-wrap">
    <PnGroup
      title={t("Get everything")}
      count={String(m.progress ? m.items.length : m.rows.length)}
      className="pn-everything"
      foot={
        <>
          <PageCard m={m} className="pn-note pn-everything-page" />
          {m.error && (
            <p className="pn-note warn" role="alert">
              {m.error}
            </p>
          )}
        </>
      }
    >
      {m.progress
        ? m.items.map((entry) => (
            <PnTile
              key={entry.item.releaseId}
              size="wide"
              tone="dark"
              className={`pn-everything-tile is-${entry.progress}`}
              glyph={entry.progress === "added" ? "check" : entry.row?.kind === "dlc" ? "layers" : "down"}
              kicker={entry.row ? rowKind(entry.row) : entry.item.kind}
              title={entry.row?.kind === "dlc" ? `${entry.row.title} · ${bytes(entry.row.release.sizeBytes)}` : entry.row ? bytes(entry.row.release.sizeBytes) : entry.item.title}
              sub={progressLabel(entry.progress)}
            />
          ))
        : m.rows.map((row) => {
            const on = m.ticked.has(row.key) && !row.installed;
            return (
              <PnTile
                key={row.key}
                size="wide"
                tone="dark"
                className="pn-everything-tile"
                glyph={row.kind === "dlc" ? "layers" : "down"}
                kicker={rowKind(row)}
                title={row.installed ? t("Installed") : row.kind === "dlc" ? `${row.title} · ${bytes(row.release.sizeBytes)}` : bytes(row.release.sizeBytes)}
                sub={rowSource(row)}
                label={rowTitle(row)}
                check={on}
                disabled={row.installed || m.pending}
                onClick={() => m.toggle(row.key)}
              />
            );
          })}
      {m.progress ? (
        <>
          {m.running && <PnTile size="wide" tone="dark" glyph="close" className="pn-everything-buttons" title={t("Stop")} disabled={m.pending} onClick={() => void m.stop()} />}
          {!m.running && <PnTile size="wide" tone="accent" glyph="check" className="pn-everything-buttons" title={t("Close")} onClick={m.dismiss} />}
          {m.running && (
            <div className="pn-tile wide pn-everything-total">
              <span className="pn-everything-line">{addingLine(m)}</span>
            </div>
          )}
        </>
      ) : (
        <>
          <PnTile
            size="wide"
            tone={m.canStart ? "accent" : "dark"}
            glyph="down"
            title={m.pending ? t("Please wait…") : t("Download all")}
            sub={m.drive ? t("{size} · to {drive}", { size: bytes(m.total), drive: placeName(m.drive.label) }) : bytes(m.total)}
            disabled={!m.canStart}
            onClick={() => void m.start()}
          />
          <div className="pn-tile wide pn-everything-total">
            <span className="pn-everything-line">{totalLine(m)}</span>
            <span className="pn-everything-fine">{t("Installs in order: game, then DLC.")}</span>
            {m.blocker && <span className="pn-everything-fine warn">{m.blocker}</span>}
          </div>
        </>
      )}
    </PnGroup>
    </div>
  );
}
