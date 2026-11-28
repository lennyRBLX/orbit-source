import type { Installation } from "./Installation";
import { formatNumber, t } from "./i18n";
export interface AddonRequirements {
  titleIds: string[];
  edition?: string | null;
  minimumVersion?: string | null;
  notes?: string | null;
}
export interface AddonIdentity {
  addonId?: string;
  baseGameId?: string;
  baseGameTitle?: string;
  addonType?: "expansion" | "content-pack" | "activation" | "bundle";
  contentId?: string;
  requirements?: AddonRequirements;
  contents?: string[];
}
export interface Release extends AddonIdentity {
  platform?: "ps4" | "ps5";
  homebrew?: boolean;
  id: string;
  gameId: string;
  titleId: string;
  title: string;
  genre: string | null;
  tagline: string | null;
  description: string | null;
  sizeBytes: number;
  sizeEstimateBytes?: number;
  sizeEstimateExact?: boolean;
  provider: string;
  sourceId: SourceId;
  torboxHost?: string;
  torboxHosts?: string[];
  torboxCached?: boolean;
  torboxSizeBytes?: number;
  torboxPending?: boolean;
  mirrorCount?: number;
  providerMetadata?: boolean;
  providerMissing?: boolean;
  // An unresolved provider page can omit the file type.
  format: "FFPFSC" | "FFPFS" | "FFPKG" | "exFAT" | "FPKG" | "PKG" | "Game folder" | "Unknown";
  contentFormat?: "LZ4";
  assetCompression?: "ZSTD";
  // Downloaded as an archive and extracted on the drive; format is what's inside.
  compression?: "zip" | "7z" | "rar";
  archivePassword?: string;
  extractedBytes?: number;
  notes?: string;
  region?: "EUR" | "USA" | "JPN" | "ASIA" | null;
  version: string | null;
  resolvedVersion?: string;
  filename: string;
  cover: string;
  hero: string;
  coverFallback?: string | null;
  heroFallback?: string | null;
  artworkLayout: "wide" | "ambient";
  publisher: string | null;
  releaseDate: string | null;
  addedAt?: string | null;
  newOnOrbitRank?: number | null;
  insights?: import("./insights").GameInsights;
  tags?: string[]; // Browse tags, as browse.ts TAGS names them
  consoleVerified: boolean;
  browserAvailable?: boolean;
  directAvailable?: boolean;
  /* The firmware an option needs, from the console's service; see src/firmware.ts. */
  requiredFirmware?: string;
  maxFirmware?: string;
  firmwareIssue?: "ps5-package" | "newer-firmware";
  filenameHint?: string;
  formatHint?: string;
  compressionHint?: string;
  hasArchivePassword?: boolean;
  edition?: string;
  metadataStatus?: string;
  metadataSizeOnly?: boolean;
}
// Get everything's Download all (GET/POST /api/v1/bundles): one at a time, items added in order.
export interface BundleItem {
  releaseId: string;
  title: string;
  kind: "game" | "dlc";
  state: "pending" | "adding" | "page" | "added" | "failed" | "skipped";
  jobId: string;
  message: string;
}
export interface Bundle {
  id: string;
  state: "idle" | "running" | "page" | "done" | "stopped";
  message: string;
  items: BundleItem[];
}
export interface Game extends AddonIdentity {
  platform?: "ps4" | "ps5";
  homebrew?: boolean;
  id: string;
  title: string;
  genre: string | null;
  tagline: string | null;
  description: string | null;
  cover: string;
  hero: string;
  coverFallback?: string | null;
  heroFallback?: string | null;
  artworkLayout: "wide" | "ambient";
  publisher: string | null;
  releaseDate: string | null;
  addedAt: string | null;
  newOnOrbitRank?: number | null;
  insights?: import("./insights").GameInsights;
  releases: Release[];
}
export type SourceId = "vikingfile" | "archive" | "akirabox" | "datanodes" | "1fichier" | "remote";
export const releaseProvider = (release: Release) => {
  const name = release.sourceId === "remote" && release.provider === "Community catalogues"
    ? t("Community catalogues") : release.provider;
  return release.mirrorCount && release.mirrorCount > 1
    ? name + " · " + t("{count} mirrors", { count: release.mirrorCount })
    : name;
};
export const formatVersion = (version?: string | null) =>
  (version || "").trim().replace(/^(?:v+\s*)?(?=\d)/i, "v");
export const releaseVersion = (release: Release) =>
  formatVersion(release.version) || formatVersion(release.resolvedVersion);
export const torboxSizeAllowed = (size?: number) => !size || size >= 500_000_000;
export function releaseSize(release: Release, useTorbox = false) {
  if (useTorbox && release.torboxSizeBytes && torboxSizeAllowed(release.torboxSizeBytes))
    return bytes(release.torboxSizeBytes);
  if (release.sizeBytes || release.sourceId !== "remote") return bytes(release.sizeBytes);
  return release.sizeEstimateBytes
    ? `${release.sizeEstimateExact ? "" : "≈"}${bytes(release.sizeEstimateBytes)}`
    : t("Unknown size");
}
export interface Storage {
  id: string;
  label: string;
  path: string;
  freeBytes: number;
  totalBytes: number;
  pendingBytes: number;
  projectedFreeBytes: number;
  external: boolean;
}
export interface Job extends AddonIdentity {
  removingFiles?: boolean;
  removalError?: string;
  requiredFirmware?: string;
  maxFirmware?: string;
  firmwareIssue?: "ps5-package" | "newer-firmware";
  firmwareSource?: string;
  consoleFirmware?: string;
  backport?: boolean;
  installation?: Installation | null;
  delivery?: "direct" | "torbox";
  deliveryPhase?: string;
  titleId: string;
  gameId: string;
  format: string;
  path: string;
  title: string;
  order: number;
  id: string;
  releaseId: string;
  storageId: string;
  filename: string;
  status:
    | "queued"
    | "downloading"
    | "paused"
    | "retrying"
    | "verifying"
    | "waiting-extraction"
    | "inspecting"
    | "extracting"
    | "complete"
    | "cancelled"
    | "error";
  received: number;
  total: number;
  speed: number;
  retryAt: number;
  error: string;
  verification: string;
  canRefreshLink?: boolean;
  // Downloads from a link the user pasted (sourceId "link"): its host, never the link.
  sourceId?: "link";
  linkHost?: string;
  canReplaceLink?: boolean;
  compression?: string;
  canEnterArchivePassword?: boolean;
  extracted?: number;
  extractTotal?: number;
  detectedFormat?: string;
  output?: string;
  archiveKept?: boolean;
  // Added while extraction was off: the archive itself is the download.
  keepPacked?: boolean;
  // Get everything: installs by itself, after installAfter's job when set.
  autoInstall?: boolean;
  installAfter?: string;
}
export interface LibraryGame {
  titleId: string;
  title: string;
  platform: "ps5" | "ps4" | "unknown";
  format: string;
  path: string;
  runtimePath: string;
  sourceKey: string;
  location: string;
  installed: boolean;
  mounted: boolean;
  onDrive: boolean;
  managed: boolean;
  canManageSource: boolean;
  installedSizeBytes: number | null;
  sizeBytes: number | null;
  sizeStatus: "ready" | "unknown" | "unavailable";
  cover?: string;
  coverFallback?: string | null;
  /** Listed by Orbit because the running ShadowMount doesn't list installed PKGs. */
  foundByOrbit?: boolean;
}
export interface LibraryAction {
  id: string;
  action: string;
  titleId: string;
  state: "queued" | "running" | "complete" | "error";
  message: string;
}
export interface StorageJob {
  id: number;
  operation: string;
  state: string;
  titleId: string;
  source: string;
  destination: string;
  error: string;
  result: number;
  totalBytes: number;
  processedBytes: number;
  speed: number;
  active: boolean;
  cancellable: boolean;
  cancelRequested: boolean;
}
export interface LibrarySnapshot {
  status: "idle" | "ready" | "unavailable" | "unsupported" | "error";
  message: string;
  provider: string;
  providerVersion: string;
  busy: boolean;
  stale: boolean;
  checkedAt: number;
  updatedAt: number;
  refreshAfter: number;
  games: LibraryGame[];
  capabilities: string[];
  action: LibraryAction | null;
  storageJob: StorageJob | null;
  storageError: string;
  storageBusy: boolean;
  // Automatic work: a scan ShadowMount is running, game sizes being measured.
  scanning?: boolean;
  measuring?: boolean;
}
export interface LibraryDrive {
  id: string;
  path: string;
  mountPoint: string;
  label: string;
  filesystem: string;
  readOnly: boolean;
  totalBytes: number;
  freeBytes: number;
  usedBytes: number;
}
// Files: what is on the PS5's drives, by drive and path relative to its top.
export type FileType = "folder" | "game" | "archive" | "package" | "image" | "file";
export interface FileEntry {
  name: string;
  folder: boolean;
  type: FileType;
  // Files only; a folder's size is measured on request.
  sizeBytes: number | null;
  modifiedAt: number;
  // Why it can't be changed: "", or drive, orbit, download, game, install or busy.
  locked: string;
}
export interface FileListing {
  storageId: string;
  path: string;
  entries: FileEntry[];
  // More entries than Orbit lists in one folder.
  truncated: boolean;
}
export interface FileOperation {
  id: string;
  action: "copy" | "move" | "delete" | "unpack" | "install" | "measure" | "verify" | "repair";
  verification?: import("./GameVerification").Verification;
  // password: an archive needs one that Orbit doesn't have.
  state: "measuring" | "running" | "paused" | "password" | "complete" | "failed" | "cancelled";
  name: string;
  storageId: string;
  path: string;
  toStorageId: string;
  toPath: string;
  processedBytes: number;
  totalBytes: number;
  speed: number;
  error: string;
  // Measure: the folder's size.
  sizeBytes: number | null;
}
export interface LibraryStorage {
  drives: LibraryDrive[];
  destinations: LibraryDrive[];
  busy: boolean;
  stale: boolean;
  updatedAt: number;
  error: string;
}
export interface LibraryTarget {
  titleId?: string;
  path?: string;
  title: string;
  request: number;
}
export interface System {
  catalogueRevision: number;
  remoteGeneration?: number;
  instanceId?: string;
  name: string;
  version: string;
  platform: "desktop" | "ps5" | "fixture";
  paired: boolean;
  stateHealthy: boolean;
  preferredStorage: string;
  deleteArchives?: boolean;
  // Off: new ZIP, 7z and RAR downloads stay archives (unset on older services).
  extractArchives?: boolean;
  downloadNotifications?: boolean;
  notificationEvents?: Record<NotificationEvent, boolean>;
  deletePackages?: boolean;
  historyRemovalAvailable?: boolean;
  historyFiles?: "ask" | "keep" | "delete";
  power?: { available: boolean; enabled: boolean; armed: boolean; jobId: string; error: string; seconds: number; state: "off" | "waiting" | "countdown" | "stopping" };
  ntfyEnabled?: boolean;
  concurrentDownloads?: number;
  downloadConnections?: number;
  downloadRetries?: number;
  downloadRetryDelay?: number;
  downloadStallTimeout?: number;
  downloadMinSpeed?: number;

  consoleValidated: boolean;
  targetFirmware: string;
  firmware?: string;
  ps5PackageFirmware?: string;
  // 1: GET/POST /api/v1/bundles (Get everything). Unset on older services.
  bundleProtocol?: number;
  localSessionAvailable: boolean;
  pairNotificationAvailable: boolean;
  httpPort: number;
  launcherStatus: "checking" | "ready" | "error" | "not-included";
}
export interface AutoStart {
  available: boolean;
  savedPath?: string;
  saved?: boolean;
  preferencesError?: string;
  payloadManager?: {
    installed: boolean;
    listed: boolean;
    managed: boolean;
    enabled: boolean;
    switchOn: boolean;
    otherEntries: number;
  };
  autoloadTxt?: { files: { path: string; enabled: boolean }[] };
  homebrewLauncher?: { installed: boolean; listed: boolean; managed: boolean };
  etaHEN?: { installed: boolean };
}
// "RAR · PKG" for a compressed option, otherwise its format.
export function formatLabel(r: Pick<Job, "format" | "compression">) {
  const format = r.format === "Unknown" ? "" : r.format;
  if (!r.compression) return format || t("Download");
  const kind = r.compression === "7z" ? "7z" : r.compression.toUpperCase();
  return format ? `${kind} · ${format}` : kind;
}
const tenths = { minimumFractionDigits: 1, maximumFractionDigits: 1 };
// 49.4 GB in English, 49,4 GB in German, 49,4 ГБ in Russian, 49,4 Go in French.
export const bytes = (n: number) => {
  const giga = n >= 1e9;
  const value = giga ? n / 1e9 : n / 1e6;
  try {
    return formatNumber(value, {
      style: "unit",
      unit: giga ? "gigabyte" : "megabyte",
      unitDisplay: "short",
      ...tenths,
    });
  } catch {
    // Browsers without unit formatting keep the English abbreviation.
    return `${formatNumber(value, tenths)} ${giga ? "GB" : "MB"}`;
  }
};

export interface PspkgChoice {
  id: string;
  gameName?: string;
  label: string;
  number: number;
  edition: string;
  titleId: string;
  version: string;
  region: string;
  format: string;
  compression: string;
  reason: string;
  group?: string;
  filename?: string;
  sizeBytes?: number;
  sizeExact?: boolean;
  hasArchivePassword?: boolean;
  providerHost?: string;
  torboxAvailable?: boolean;
  torboxCached?: boolean;
  torboxSizeBytes?: number;
}

export interface BrowserSession {
  pspkgReleaseId?: string;
  pspkgLinks?: PspkgChoice[];
  pspkgGameId?: string;
  pspkgStatus?: string;
  accountPage?: boolean;
  vikingAccountAvailable?: boolean;
  vikingBrowserState?: "signed-in" | "signed-out" | "unknown";
  vikingNativeState?: "signed-in" | "signed-out" | "unknown";
  available: boolean;
  active: boolean;
  id: string;
  title: string;
  state:
    | "idle"
    | "opening"
    | "waiting"
    | "waiting-link"
    | "validating"
    | "links-ready"
    | "queued"
    | "failed"
    | "cancelled";
  message: string;
  jobId: string;
  // Set when the TV app opened the page in its own window: it reports the result itself.
  clientId?: string;
}
// What Orbit can notify about, on the PS5 and through ntfy (/settings/notifications).
export type NotificationEvent = "downloadFinished" | "downloadFailed" | "installFinished" | "installFailed" | "updateAvailable";
