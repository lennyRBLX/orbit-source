export interface GameInsights {
  metacritic?: { score: number; platform: string; url: string; checkedAt: string; snapshotAt?: string; dataSource?: string; license?: string };
  howLongToBeat?: { mainMinutes?: number; extrasMinutes?: number; completionistMinutes?: number; edition: string; url: string; checkedAt: string; snapshotAt?: string; dataSource?: string; license?: string };
}
export const scoreChoices = ["", "60", "70", "80", "90"];
export const timeChoices = ["", "under5", "5to10", "10to20", "20to40", "40plus"];
export const timeLabels = [/* i18n */ "Any playtime", /* i18n */ "Under 5 hours", /* i18n */ "5–10 hours", /* i18n */ "10–20 hours", /* i18n */ "20–40 hours", /* i18n */ "40+ hours"];
export const playStyles = ["main", "extras", "completionist"];
export const playStyleLabels = [/* i18n */ "Main Story", /* i18n */ "Main + Extras", /* i18n */ "Completionist"];
export const insightSorts = [
  { value: "score-desc", label: /* i18n */ "Highest score" },
  { value: "score", label: /* i18n */ "Lowest score" },
  { value: "time", label: /* i18n */ "Shortest playtime" },
  { value: "time-desc", label: /* i18n */ "Longest playtime" },
];
export function playtime(insights?: GameInsights, style = "main") {
  const h = insights?.howLongToBeat;
  return (style === "extras" ? h?.extrasMinutes : style === "completionist" ? h?.completionistMinutes : h?.mainMinutes) ?? null;
}
export function matchesInsights(insights: GameInsights | undefined, minimum = "", range = "", style = "main") {
  if (minimum && (insights?.metacritic?.score == null || insights.metacritic.score < Number(minimum))) return false;
  const minutes = playtime(insights, style);
  if (!range) return true;
  if (minutes == null) return false;
  return range === "under5" ? minutes < 300 : range === "5to10" ? minutes >= 300 && minutes < 600 :
    range === "10to20" ? minutes >= 600 && minutes < 1200 : range === "20to40" ? minutes >= 1200 && minutes < 2400 : range === "40plus" && minutes >= 2400;
}
// Unknown values stay last in either direction, rather than becoming zero.
export function compareKnown(a: number | null | undefined, b: number | null | undefined, descending = false) {
  return a == null ? b == null ? 0 : 1 : b == null ? -1 : (descending ? b - a : a - b);
}
