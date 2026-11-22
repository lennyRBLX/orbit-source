import { SupportPanel } from "../Support";
import { VikingAccount } from "../VikingAccount";
// Shared settings categories in all four layouts; Store hosts this page in its App settings dialog.
import { useEffect, useRef, useState, type ReactNode } from "react";
import type { SourceSettings, System } from "../types";
import { languages, savedLanguage, t, tn } from "../i18n";
import { interfaces, useInterfacePrefs } from "./choice";
import { Appearance } from "./Appearance";
import { LanguageSetting } from "../LanguageSetting";
import { CatalogueUpdates } from "../CatalogueUpdates";
import { Updates } from "../Updates";
import { TvApp } from "../TvApp";
import { Debrid } from "../Debrid";
import { ArchiveAccount } from "../ArchiveAccount";
import { Diagnostics } from "../Diagnostics";
import { PayloadManagers, StartAutomatically, useAutoStart } from "../AutoStart";
import { Glyph, type GlyphName } from "./Glyph";

export type SettingsSection =
  | "appearance"
  | "language"
  | "sources"
  | "downloads"
  | "storage"
  | "notifications"
  | "pair"
  | "catalogue"
  | "updates"
  | "tvapp"
  | "zero"
  | "torbox"
  | "archive"
  | "viking"
  | "payload"
  | "autostart"
  | "support"
  | "diagnostics";

export const settingsGroups: { label: string; sections: { id: SettingsSection; label: string; glyph: GlyphName }[] }[] = [
  { label: /* i18n */ "Settings", sections: [
    { id: "downloads", label: /* i18n */ "Downloads", glyph: "down" },
    { id: "storage", label: /* i18n */ "Storage & cleanup", glyph: "drive" },
    { id: "sources", label: /* i18n */ "Sources & accounts", glyph: "cloud" },
    { id: "pair", label: /* i18n */ "Devices", glyph: "link" },
    { id: "appearance", label: /* i18n */ "Appearance", glyph: "palette" },
    { id: "notifications", label: /* i18n */ "Notifications", glyph: "bell" },
    { id: "catalogue", label: /* i18n */ "Game catalogue", glyph: "refresh" },
    { id: "updates", label: /* i18n */ "Updates & startup", glyph: "update" },
    { id: "diagnostics", label: /* i18n */ "Diagnostics & about", glyph: "bug" },
    { id: "support", label: /* i18n */ "Support Orbit", glyph: "heart" },
  ] },
];
const aliases: Partial<Record<SettingsSection, SettingsSection>> = {
  language: "appearance", tvapp: "updates", payload: "updates", autostart: "updates",
  torbox: "sources", archive: "sources", viking: "sources", zero: "pair",
};
export const settingsSections = settingsGroups.flatMap((g) => g.sections);

export function SettingsPage({
  section,
  choose,
  system,
  sources,
  storage,
  downloads,
  notifications,
  zero,
  pairing,
  updateAvailable,
  sourceSettings,
}: {
  section: SettingsSection;
  choose: (section: SettingsSection) => void;
  system: System | null;
  sources: ReactNode;
  storage: ReactNode;
  downloads: ReactNode;
  notifications: ReactNode;
  zero: ReactNode;
  pairing: ReactNode;
  updateAvailable: boolean;
  sourceSettings: SourceSettings | null;
}) {
  const auto = useAutoStart();
  const { prefs } = useInterfacePrefs();
  // Each section's current value, where the page already knows it.
  const language = savedLanguage();
  const values: Partial<Record<SettingsSection, string>> = {
    appearance: t(interfaces.find((i) => i.name === prefs.name)?.title || ""),
    ...(sourceSettings
      ? {
          sources: sourceSettings.options
            .filter((o) => sourceSettings.enabled.includes(o.id))
            .map((o) => o.label)
            .join(", "),
        }
      : {}),
    language: language ? languages.find((l) => l.code === language)?.name || "" : t("Automatic"),
    ...(system?.paired && system.concurrentDownloads
      ? { downloads: tn(system.concurrentDownloads, "{count} at a time", "{count} at a time") }
      : {}),
    ...(system?.notificationEvents
      ? {
          notifications:
            [system.downloadNotifications !== false && "PS5", system.ntfyEnabled && "ntfy"].filter(Boolean).join(", ") ||
            t("Off"),
        }
      : {}),
    ...(system?.catalogueRevision ? { catalogue: t("Catalogue {revision}", { revision: system.catalogueRevision }) } : {}),
    ...(system?.version ? { updates: system.version } : {}),
  };
  // Stopping Orbit for an update leaves only Updates usable, as in App settings.
  const [stopped, setStopped] = useState(false);
  const current = settingsSections.find((s) => s.id === (aliases[section] || section)) || settingsSections[0];
  // Panorama's pivots run off the page: keep the chosen one in view.
  const nav = useRef<HTMLElement>(null);
  useEffect(() => {
    if (document.activeElement === document.body) nav.current?.querySelector<HTMLElement>('[aria-current="page"]')?.focus();
  }, []);
  useEffect(() => {
    nav.current?.querySelector('[aria-current="page"]')?.scrollIntoView({ block: "nearest", inline: "nearest" });
  }, [current.id]);
  // A newly chosen section opens at its top: a long one before it may have
  // left the page scrolled past where the next one starts.
  const panel = useRef<HTMLDivElement>(null);
  useEffect(() => {
    const el = panel.current;
    let box = el?.parentElement;
    while (box && !(box.scrollHeight > box.clientHeight && /auto|scroll/.test(getComputedStyle(box).overflowY))) box = box.parentElement;
    if (!el || !box) return;
    const above = el.getBoundingClientRect().top - box.getBoundingClientRect().top;
    if (above < 0) box.scrollTop += above;
    if (aliases[section]) panel.current?.querySelector<HTMLElement>(`[data-setting="${section}"]`)?.scrollIntoView({ block: "start" });
  }, [current.id, section]);
  const startup = <><div data-setting="payload"><PayloadManagers system={system} auto={auto} /></div><div data-setting="autostart"><StartAutomatically auto={auto} /></div>
    {auto.error && <p className="form-error" role="alert">{auto.error}</p>}</>;
  const accounts = <>{sources}<div data-setting="torbox"><Debrid /></div><div data-setting="archive"><ArchiveAccount /></div><div data-setting="viking"><VikingAccount /></div></>;
  const updatePanels = <><Updates onStopped={() => setStopped(true)} />{!stopped && <><div data-setting="tvapp"><TvApp /></div>{startup}</>}</>;
  const content: Partial<Record<SettingsSection, ReactNode>> = {
    appearance: <><Appearance page /><div data-setting="language"><LanguageSetting /></div></>,
    sources: accounts,
    downloads,
    storage,
    notifications,
    pair: <>{pairing}<div data-setting="zero">{zero}</div></>,
    catalogue: <CatalogueUpdates />,
    updates: updatePanels,
    diagnostics: <><dl className="update-versions"><div><dt>{t("Download service")}</dt><dd>{system?.version || t("Unknown")}</dd></div></dl><Diagnostics /></>,
    support: <SupportPanel />,
  };
  return (
    <section className="settings-page" aria-label={t("Settings")}>
      <nav className="settings-sections" ref={nav} aria-label={t("Settings sections")}>
        {settingsGroups.map((group) => (
          <div className={`settings-group${group.sections.some((s) => s.id === current.id) ? " current" : ""}`} key={group.label}>
            <p className="settings-group-label">{t(group.label)}</p>
            {group.sections.map((s) => (
              <button
                key={s.id}
                className="settings-section"
                aria-current={s.id === current.id ? "page" : undefined}
                disabled={stopped && s.id !== "updates"}
                onClick={() => choose(s.id)}
              >
                <span className="settings-section-icon">
                  <Glyph name={s.glyph} size={18} />
                </span>
                <span className="settings-section-label">{["TorBox", "Archive.org", "Orbit Zero"].includes(s.label) ? s.label : t(s.label)}</span>
                {values[s.id] && <span className="settings-section-value">{values[s.id]}</span>}
                {s.id === "updates" && updateAvailable && (
                  <i className="nav-dot" role="img" aria-label={t("Update available")} />
                )}
              </button>
            ))}
          </div>
        ))}
      </nav>
      <div ref={panel} className={`settings-panel settings-panel-${current.id}`} key={current.id}>
        {/* The other panels open with their own heading. */}
        {["downloads", "storage", "notifications", "sources", "pair", "updates", "diagnostics"].includes(current.id) && <h1 className="settings-title">{t(current.label)}</h1>}
        {stopped && current.id !== "updates" ? content.updates : content[current.id]}
      </div>
    </section>
  );
}
