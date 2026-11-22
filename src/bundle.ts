// Get everything: which add-ons go with a game option, the checklist's rows and
// totals, and the request that adds them to Downloads (POST /api/v1/bundles).
// Labels are left to each look.
import type { Bundle, BundleItem, Game, Job, LibraryGame, Release, Storage } from "./types";
import { preferredRelease } from "./downloadChoice";
import { defaultDrive, downloadSpacePlan } from "./collection";
import { gamePlatform } from "./platform";

type Choice = { source: string; format: string } | null;

const sameTitle = (a: string, b: string) => !!a && a.toUpperCase() === b.toUpperCase();

// The option a download starts on, as the Download button chooses it.
export function pickOption<T extends Release>(releases: T[], last?: Choice): T {
  return (last === undefined ? preferredRelease(releases) : preferredRelease(releases, undefined, last)) as T;
}

// The add-ons for a game option: those with options for the same title ID
// (PPSA or CUSA) on the same platform.
export function addonOptions(addons: Game[], release: Release) {
  const platform = gamePlatform(release);
  return addons
    .map((addon) => ({
      addon,
      releases: addon.releases.filter((r) => sameTitle(r.titleId, release.titleId) && gamePlatform(r) === platform),
    }))
    .filter((entry) => entry.releases.length > 0);
}

// The Library copy that counts as this game installed.
export function installedCopy(library: LibraryGame[], release: Release) {
  const platform = gamePlatform(release);
  return library.find((g) => sameTitle(g.titleId, release.titleId) && (g.platform === platform || g.platform === "unknown") && g.installed);
}

export interface ChecklistRow {
  key: string;
  kind: "game" | "dlc";
  release: Release;
  // The add-on's name (DLC rows).
  title: string;
  // Game row only: Library has it installed, so it is left out.
  installed: boolean;
}

// Get everything's rows, in install order: the game option the Download button
// uses, then each add-on for the same title ID.
export function checklistRows({
  release,
  addons,
  library,
  last,
}: {
  release: Release;
  addons: Game[];
  library: LibraryGame[];
  last?: Choice;
}): ChecklistRow[] {
  if (release.addonId || !release.titleId) return [];
  const rows: ChecklistRow[] = [
    { key: "game", kind: "game", release, title: "", installed: !!installedCopy(library, release) },
  ];
  for (const { addon, releases } of addonOptions(addons, release))
    rows.push({ key: `dlc:${addon.id}`, kind: "dlc", release: pickOption(releases, last), title: addon.title, installed: false });
  return rows;
}

// Get everything is offered when there is more than the game to get.
export const hasExtras = (rows: ChecklistRow[]) => rows.some((r) => r.kind !== "game");

// Ticked unless it is the installed game.
export const defaultTicks = (rows: ChecklistRow[]) =>
  new Set(rows.filter((r) => !(r.kind === "game" && r.installed)).map((r) => r.key));

export const chosenRows = (rows: ChecklistRow[], ticked: ReadonlySet<string>) =>
  rows.filter((r) => ticked.has(r.key) && !(r.kind === "game" && r.installed));

// What a set of downloads needs on a drive, as the Download screen counts it:
// free space after unfinished downloads, less each item not queued there yet.
// Room to unpack an archive is checked when it is unpacked.
export function spaceFor(releases: Release[], drive: Storage | undefined, jobs: Job[]) {
  const total = releases.reduce((n, r) => n + r.sizeBytes, 0);
  const free = drive ? drive.projectedFreeBytes : 0;
  const need = drive
    ? releases.reduce((n, r) => (downloadSpacePlan(r, drive, jobs).planned ? n : n + r.sizeBytes), 0)
    : 0;
  return { total, free, enough: !!drive && free - need >= 16 * 1024 ** 2 };
}

// The drive a set of downloads goes to: Download's own rule, for one item or
// for all of them at once.
export function driveFor(drives: Storage[], preferred: string | undefined, releases: Release[], jobs: Job[]) {
  if (!releases.length) return undefined;
  if (releases.length === 1) return defaultDrive(drives, preferred, releases[0], jobs);
  const all: Release = { ...releases[0], id: "", sizeBytes: releases.reduce((n, r) => n + r.sizeBytes, 0) };
  return defaultDrive(drives, preferred, all, jobs);
}

// The console adds 1 to 64 items in one Download all.
export const BUNDLE_LIMIT = 64;

export function bundleRequest(
  storageId: string,
  items: { release: Release; delivery?: "direct" | "torbox" }[],
  installInOrder: boolean,
  returnToOrbit = true,
) {
  return {
    action: "start",
    storageId,
    items: items.slice(0, BUNDLE_LIMIT).map((i) => ({ releaseId: i.release.id, ...(i.delivery ? { delivery: i.delivery } : {}) })),
    installInOrder,
    ...(returnToOrbit ? { returnToOrbit: true } : {}),
  };
}

export const bundleRunning = (b: Bundle | null | undefined) => !!b && (b.state === "running" || b.state === "page");

// A row's place in a Download all: added, its page open on the PS5, waiting
// its turn (adding: being added now), or not added (failed or skipped).
export type RowProgress = "added" | "page" | "adding" | "waiting" | "not-added";
export function rowProgress(item: Pick<BundleItem, "state"> | undefined): RowProgress {
  switch (item?.state) {
    case "added":
      return "added";
    case "page":
      return "page";
    case "adding":
      return "adding";
    case "failed":
    case "skipped":
      return "not-added";
    default:
      return "waiting";
  }
}

// The bundle these rows started (or one running with any of them).
export function bundleFor(bundle: Bundle | null, releaseIds: string[], startedId = "") {
  if (!bundle || !bundle.items.length) return null;
  if (startedId && bundle.id === startedId) return bundle;
  return bundleRunning(bundle) && bundle.items.some((i) => releaseIds.includes(i.releaseId)) ? bundle : null;
}

export const addedCount = (bundle: Bundle) => bundle.items.filter((i) => i.state === "added").length;
