// Which download option a game page starts on: one matching the Browse filters the game was
// found with, else the source and format downloaded last time, else one that downloads without
// a provider page, then catalogue order.
import type { Release } from "./types";
import { filtersOptions, releaseFormat, releaseMatches, type BrowseFilters } from "./browse";

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
// torbox: the sources TorBox can download now, for Source's "TorBox available" choice.
export function preferredRelease(releases: Release[], filters?: BrowseFilters, last = lastChoice(), torbox?: ReadonlySet<string>): Release {
  const filtered = !!filters && filtersOptions(filters);
  const rank = (r: Release) => [
    filtered && releaseMatches(r, filters!, torbox) ? 0 : 1,
    last && r.sourceId === last.source ? (releaseFormat(r) === last.format ? 0 : 1) : 2,
    r.browserAvailable ? 1 : 0,
  ];
  return releases
    .map((r, i) => ({ r, i, k: rank(r) }))
    .sort((a, b) => a.k[0] - b.k[0] || a.k[1] - b.k[1] || a.k[2] - b.k[2] || a.i - b.i)[0].r;
}
