import { t } from "./i18n";
// Older cached PS5 catalogues predate the explicit platform field.
export function gamePlatform(value: { platform?: "ps4" | "ps5"; titleId?: string }): "ps4" | "ps5" {
  return value.platform ?? (value.titleId?.startsWith("CUSA") ? "ps4" : "ps5");
}

export function gamePlatformLabel(game: { platform?: "ps4" | "ps5"; titleId?: string; homebrew?: boolean }) {
  return game.homebrew ? t("Homebrew") : gamePlatform(game).toUpperCase();
}
