import { formatVersion, releaseVersion, torboxSizeAllowed, type PspkgChoice, type Release } from "./types";
import { filtersOptions, releaseFormat, releaseHasTorboxHost, releaseMatches, type BrowseFilters } from "./browse";

const KEY = "orbit.download-choice.v1";
type Choice = { source: string; format: string };

function lastChoice(): Choice | null {
  try {
    const value = JSON.parse(localStorage.getItem(KEY) || "null");
    return value && typeof value.source === "string" && typeof value.format === "string" ? value : null;
  } catch {
    return null;
  }
}
export function rememberChoice(r: Release) {
  try {
    localStorage.setItem(KEY, JSON.stringify({ source: r.sourceId, format: releaseFormat(r) }));
  } catch {
    // A private window or blocked storage only loses the preference.
  }
}
// Start on an option matching the Browse filters, then prefer TorBox, the last
// source and format, and direct downloads in that order.
// torbox: the sources TorBox can download now.
export function preferredRelease(releases: Release[], filters?: BrowseFilters, last = lastChoice(), torbox?: ReadonlySet<string>): Release {
  const filtered = !!filters && filtersOptions(filters);
  const rank = (r: Release) => {
    const covered = torbox && torboxSizeAllowed(r.torboxSizeBytes) &&
      (!r.providerMissing || r.torboxCached) && releaseHasTorboxHost(r, torbox);
    return [
      filtered && releaseMatches(r, filters!, torbox) ? 0 : 1,
      covered ? (r.torboxCached ? 0 : 1) : 2,
      last && r.sourceId === last.source ? (releaseFormat(r) === last.format ? 0 : 1) : 2,
      r.browserAvailable ? 1 : 0,
    ];
  };
  return releases
    .map((r, i) => ({ r, i, k: rank(r) }))
    .sort((a, b) => a.k[0] - b.k[0] || a.k[1] - b.k[1] || a.k[2] - b.k[2] ||
      a.k[3] - b.k[3] || a.i - b.i)[0].r;
}

const versionOrder = new Intl.Collator("en", { numeric: true, sensitivity: "base" });
const versionKey = (version: string) => version.replace(/^(v\d+(?:\.\d+)*?)(?:\.0+)+$/, "$1");
const torboxRank = (available?: boolean, cached?: boolean) => available ? (cached ? 0 : 1) : 2;

function formatRank(format: string) {
  switch (format.toUpperCase()) {
    case "PKG":
    case "FPKG":
    case "FFPKG": return 0;
    case "ZSTD": return 1;
    case "LZ4": return 2;
    case "GAME FOLDER": return 3;
    default: return 4;
  }
}

export function sortDownloadLinks<T extends Release | PspkgChoice>(
  links: readonly T[],
  torboxAvailable: (link: T) => boolean | undefined,
): T[] {
  return links.map((link) => {
    const catalogue = "sourceId" in link;
    return {
      link,
      version: versionKey(catalogue ? releaseVersion(link) : formatVersion(link.version)),
      torbox: torboxRank(torboxAvailable(link), link.torboxCached),
      format: formatRank(catalogue ? releaseFormat(link) : link.format),
    };
  }).sort((a, b) => Number(!a.link.titleId?.trim()) - Number(!b.link.titleId?.trim()) ||
    versionOrder.compare(b.version, a.version) ||
    a.torbox - b.torbox || a.format - b.format).map(({ link }) => link);
}
