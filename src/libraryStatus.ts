import type { LibraryGame } from "./types";

export const libraryViews = [/* i18n */ "All games", /* i18n */ "Available now", /* i18n */ "Missing source"] as const;
export type LibraryView = (typeof libraryViews)[number];
export const matchesLibraryView = (game: LibraryGame, view: string) =>
  view === "Available now" ? game.onDrive : view === "Missing source" ? !game.onDrive : true;

// Installation and mounting are independent badges. Availability is one
// mutually exclusive split, counted after the other active Library filters.
export function libraryViewOptions(games: LibraryGame[], query = "", location = "") {
  const text = query.trim().toLowerCase();
  const matching = games.filter(g => (!location || g.location === location) &&
    `${g.title} ${g.titleId} ${g.path} ${g.platform}`.toLowerCase().includes(text));
  return libraryViews.map(value => ({ value, label: value, count: matching.filter(g => matchesLibraryView(g, value)).length }));
}
