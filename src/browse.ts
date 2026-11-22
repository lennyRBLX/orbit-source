import { matchesInsights, compareKnown, playtime } from "./insights";
import type { Game, Release } from "./types";
import { gamePlatform } from "./platform";
// The tags Browse can filter by, as the catalogue names them (tools/catalog.py
// KNOWN_TAGS). Tags this build has no label for are ignored.
export const TAGS: Record<string, string> = {
  exclusive: /* i18n */ "PlayStation exclusive",
  "local-coop": /* i18n */ "Local co-op",
  "split-screen": /* i18n */ "Split-screen",
  "open-world": /* i18n */ "Open world",
  "story-driven": /* i18n */ "Story-driven",
  soulslike: /* i18n */ "Soulslike",
  "psvr-supported": /* i18n */ "PSVR supported",
  "psvr2-supported": /* i18n */ "PSVR2 supported",
  "turn-based": /* i18n */ "Turn-based",
};
// The labelled tags the given games carry, in TAGS order.
export function catalogueTags(games: Game[]): string[] {
  const present = new Set(games.flatMap((g) => g.releases.flatMap((r) => r.tags || [])));
  return Object.keys(TAGS).filter((tag) => present.has(tag));
}
// Split only the catalogue's separator: Driving/Racing and Music/Rhythm are single genres.
const GENRE_ALIASES: Record<string, string> = {
  Sports: "Sport", Simulator: "Simulation", Racing: "Driving/Racing", Driving: "Driving/Racing", "Racing/Driving": "Driving/Racing",
  RPG: "Role Playing Games", "Role-playing": "Role Playing Games", "Role Playing Game (RPG)": "Role Playing Games",
  "ストラテジー": "Strategy", "シミュレーター": "Simulation",
};
// " / " separates genres, and a few catalogue rows use commas ("Action,Horror"). A bare slash
// stays: Driving/Racing and Music/Rhythm are single genres.
export const GENRE_SEPARATOR = / \/ |,/;
export function gameGenres(genre?: string | null): string[] {
  return [...new Set((genre || "").split(GENRE_SEPARATOR).map((part) => part.trim()).filter(Boolean).map((part) => GENRE_ALIASES[part] || part))];
}
export function catalogueGenres(games: Game[]): string[] {
  // Use the full catalogue, so search and other filters never remove choices.
  const counts = new Map<string, number>();
  for (const game of games)
    for (const genre of gameGenres(game.genre))
      counts.set(genre, (counts.get(genre) || 0) + 1);
  return [...counts].filter(([, count]) => count >= 10).map(([genre]) => genre).sort();
}
// Tiles mounted per step of an infinitely scrolling catalogue grid.
export const CATALOG_STEP = 96;
export interface BrowseFilters {
  query: string;
  addonType?: string;
  // Any of these; none for all.
  source: string[];
  format: string[];
  tags: string[]; // any of them
  genres: string[]; // any of them, using the game's detail metadata
  region: string;
  platform: string;
  size: string;
  collection: "all" | "favorites";
  // Leaves out games installed on the PS5 or on one of its drives (Library).
  hideInstalled: boolean;
  sort: string;
  minimumScore?: string;
  playtimeRange?: string;
  playStyle?: string;
}
// Browse's saved filters, kept on the PS5 (docs/api.md#saved-filters).
export interface SavedFilter {
  id: string;
  name: string;
  page: "games" | "addons";
  filters: BrowseFilters;
  savedAt: number;
}
// Anything set beyond a page's starting point, the search included.
export function narrowed(f: BrowseFilters, defaultSort: string) {
  return !!(
    f.minimumScore || f.playtimeRange || (f.playStyle && f.playStyle !== "main") ||
    f.query.trim() ||
    f.source.length ||
    downloadFormats(f.format).length ||
    f.tags.length ||
    f.genres?.length ||
    f.hideInstalled ||
    f.region ||
    f.platform ||
    f.size ||
    f.collection !== "all" ||
    f.addonType ||
    f.sort !== defaultSort
  );
}
export const defaultFilters: BrowseFilters = {
  query: "",
  source: [],
  format: [],
  tags: [],
  genres: [],
  region: "",
  platform: "",
  size: "",
  collection: "all",
  hideInstalled: false,
  sort: "release",
  minimumScore: "", playtimeRange: "", playStyle: "main",
};
// Explicit catalogue picks take priority over Latest releases.
export function newOnOrbitPicks(games: Game[]): Game[] {
  return games
    .filter((game) => Number.isInteger(game.newOnOrbitRank) && game.newOnOrbitRank! > 0 && game.newOnOrbitRank! <= 100)
    .sort((a, b) => a.newOnOrbitRank! - b.newOnOrbitRank! || a.title.localeCompare(b.title))
    .slice(0, 40);
}
// Catalogue picks lead the row; fill the remaining places with recent additions.
export function newlyAddedGames(games: Game[], latest: Game[] = []): Game[] {
  const latestIds = new Set(latest.map((game) => game.id));
  const eligible = games.filter((game) => !latestIds.has(game.id));
  const pinned = newOnOrbitPicks(eligible);
  const pinnedIds = new Set(pinned.map((game) => game.id));
  const recent = eligible
    .filter((game) => !pinnedIds.has(game.id))
    .map((game) => ({ game, added: Date.parse(game.addedAt || "") }))
    .filter(({ added }) => Number.isFinite(added))
    .sort((a, b) => b.added - a.added ||
      (b.game.releaseDate || "").localeCompare(a.game.releaseDate || "") || a.game.title.localeCompare(b.game.title))
    .slice(0, 40 - pinned.length)
    .map(({ game }) => game)
    .sort((a, b) => (b.releaseDate || "").localeCompare(a.releaseDate || "") || a.title.localeCompare(b.title));
  return [...pinned, ...recent];
}
// Source's "TorBox available" choice: options from the sources TorBox can download right now,
// as a game page shows them (torbox: those sourceIds; none while TorBox isn't connected).
export const TORBOX_SOURCE = "torbox";
// installed: the IDs of games in the PS5's Library, for hideInstalled.
// A download option matches when it passes every filter that applies to options; a game shows
// in Browse when one of its options does.
export function releaseMatches(r: Release, filters: BrowseFilters, torbox: ReadonlySet<string> = new Set()) {
  if (filters.platform && (r.homebrew ? "homebrew" : gamePlatform(r)) !== filters.platform) return false;
  if (filters.source.length && !filters.source.includes(r.sourceId) &&
      !(filters.source.includes(TORBOX_SOURCE) &&
        (r.torboxHosts ?? [r.torboxHost || r.sourceId]).some((host) => torbox.has(host))))
    return false;
  const formats = downloadFormats(filters.format);
  if (formats.length && !formats.includes(releaseFormat(r))) return false;
  if (filters.region && (r.region || "unknown") !== filters.region) return false;
  const gb = r.sizeBytes / 1e9;
  return (
    !filters.size ||
    (r.sizeBytes > 0 && (filters.size === "small"
      ? gb < 10
      : filters.size === "medium"
        ? gb >= 10 && gb < 50
        : filters.size === "large"
          ? gb >= 50 && gb < 100
          : gb >= 100))
  );
}
export const releaseFormat = (r: Release): string => r.format;
// The only formats Orbit downloads, archived or not; the service leaves every other one out of
// the catalogue. Format's choices, in order, with their labels.
export const DOWNLOAD_FORMATS: Record<string, string> = {
  PKG: "PKG",
  FPKG: "FPKG",
  "Game folder": /* i18n */ "Game folder",
};
// A saved filter can name a format or archive type Orbit no longer offers: ignore it.
export const downloadFormats = (values: readonly string[] = []) =>
  values.filter((value) => Object.keys(DOWNLOAD_FORMATS).includes(value));
// Any filter that narrows the options, not just the games.
export const filtersOptions = (filters: BrowseFilters) =>
  !!(filters.platform || filters.source.length || downloadFormats(filters.format).length || filters.region || filters.size);

// The filters a Browse dropdown can count: one value each, or several (any of them).
export type CountedFilter = "platform" | "region" | "size" | "addonType" | "hideInstalled" | "source" | "format" | "tags" | "genres" | "minimumScore" | "playtimeRange";
// How many games each choice of one filter would show, with the other filters and the search
// as they are. A multiple-choice filter counts each choice on its own; "" is its "All …" choice.
export function choiceCounts(
  games: Game[],
  filters: BrowseFilters,
  favorites: ReadonlySet<string>,
  installed: ReadonlySet<string>,
  key: CountedFilter,
  values: string[],
  torbox: ReadonlySet<string> = new Set(),
): Map<string, number> {
  const multiple = key === "source" || key === "format" || key === "tags" || key === "genres";
  return new Map(values.map((value) => {
    const choice = key === "hideInstalled" ? value === "hide" : multiple ? (value ? [value] : []) : value;
    return [value, matchGames(games, { ...filters, [key]: choice }, favorites, installed, torbox).length];
  }));
}

// The games with an option matching every filter, each with its smallest matching option, in
// catalogue order.
function matchGames(
  games: Game[],
  filters: BrowseFilters,
  favorites: ReadonlySet<string>,
  installed: ReadonlySet<string>,
  torbox: ReadonlySet<string>,
) {
  const query = filters.query.trim().toLowerCase();
  return games.flatMap((game) => {
    if (
      query &&
      !`${game.title} ${game.baseGameTitle || ""} ${game.releases.map((r) => r.titleId).join(" ")}`
        .toLowerCase()
        .includes(query)
    )
      return [];
    if (!matchesInsights(game.insights, filters.minimumScore, filters.playtimeRange, filters.playStyle)) return [];
    if (filters.collection === "favorites" && !favorites.has(game.id))
      return [];
    if (filters.hideInstalled && installed.has(game.id)) return [];
    if (filters.tags.length && !game.releases.some((r) => r.tags?.some((tag) => filters.tags.includes(tag)))) return [];
    if (filters.genres?.length && !gameGenres(game.genre).some((genre) => filters.genres.includes(genre))) return [];
    if (filters.addonType && game.addonType !== filters.addonType) return [];
    const releases = game.releases.filter((r) => releaseMatches(r, filters, torbox));
    return releases.length
      ? [{ game, size: Math.min(...releases.map((r) => r.sizeBytes)) }]
      : [];
  });
}
export function browseGames(
  games: Game[],
  filters: BrowseFilters,
  favorites: ReadonlySet<string>,
  installed: ReadonlySet<string> = new Set(),
  torbox: ReadonlySet<string> = new Set(),
): Game[] {
  const matches = matchGames(games, filters, favorites, installed, torbox);
  matches.sort((a, b) => {
    const title = a.game.title.localeCompare(b.game.title);
    switch (filters.sort) {
      case "score": case "score-desc": return compareKnown(a.game.insights?.metacritic?.score, b.game.insights?.metacritic?.score, filters.sort === "score-desc") || title;
      case "time": case "time-desc": return compareKnown(playtime(a.game.insights,filters.playStyle),playtime(b.game.insights,filters.playStyle),filters.sort === "time-desc") || title;
      case "added":
        return (b.game.addedAt || "").localeCompare(a.game.addedAt || "") || title;
      case "title-desc":
        return -title;
      case "release":
        return (
          (b.game.releaseDate || "").localeCompare(a.game.releaseDate || "") ||
          title
        );
      case "size":
        return a.size - b.size || title;
      case "size-desc":
        return b.size - a.size || title;
      default:
        return title;
    }
  });
  return matches.map((x) => x.game);
}
