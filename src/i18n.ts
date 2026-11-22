// Orbit's interface language. The English text is the key: a missing
// translation shows the English, and fixed messages from Orbit's service on
// the PS5 translate the same way. Catalogues live in i18n/<code>.json, shared
// with the TV app.
export type Language =
  | "en" | "ar" | "cs" | "da" | "de" | "el" | "es" | "fi" | "fr" | "hu" | "id" | "it" | "ja"
  | "ko" | "nb" | "nl" | "pl" | "pt-BR" | "pt-PT" | "ro" | "ru" | "sv" | "tr" | "uk"
  | "zh-Hans" | "zh-Hant";

// Each language under its own name, so people can find theirs.
export const languages: { code: Language; name: string }[] = [
  { code: "en", name: "English" },
  { code: "id", name: "Bahasa Indonesia" },
  { code: "cs", name: "Čeština" },
  { code: "da", name: "Dansk" },
  { code: "de", name: "Deutsch" },
  { code: "es", name: "Español" },
  { code: "fr", name: "Français" },
  { code: "it", name: "Italiano" },
  { code: "hu", name: "Magyar" },
  { code: "nl", name: "Nederlands" },
  { code: "nb", name: "Norsk bokmål" },
  { code: "pl", name: "Polski" },
  { code: "pt-BR", name: "Português (Brasil)" },
  { code: "pt-PT", name: "Português (Portugal)" },
  { code: "ro", name: "Română" },
  { code: "fi", name: "Suomi" },
  { code: "sv", name: "Svenska" },
  { code: "tr", name: "Türkçe" },
  { code: "el", name: "Ελληνικά" },
  { code: "ru", name: "Русский" },
  { code: "uk", name: "Українська" },
  { code: "ar", name: "العربية" },
  { code: "ja", name: "日本語" },
  { code: "ko", name: "한국어" },
  { code: "zh-Hans", name: "简体中文" },
  { code: "zh-Hant", name: "繁體中文" },
];

// A plural entry has a form per CLDR category; the English key is the
// "other" form passed to tn().
type Entry = string | Partial<Record<Intl.LDMLPluralRule, string>>;
export type Catalogue = Record<string, Entry>;
type Vars = Record<string, string | number>;

// Vite splits each catalogue into its own file, so only one is downloaded.
const catalogues: Record<
  Exclude<Language, "en">,
  () => Promise<{ default: Catalogue }>
> = {
  ar: () => import("../i18n/ar.json"),
  cs: () => import("../i18n/cs.json"),
  da: () => import("../i18n/da.json"),
  de: () => import("../i18n/de.json"),
  el: () => import("../i18n/el.json"),
  es: () => import("../i18n/es.json"),
  fi: () => import("../i18n/fi.json"),
  fr: () => import("../i18n/fr.json"),
  hu: () => import("../i18n/hu.json"),
  id: () => import("../i18n/id.json"),
  it: () => import("../i18n/it.json"),
  ja: () => import("../i18n/ja.json"),
  ko: () => import("../i18n/ko.json"),
  nb: () => import("../i18n/nb.json"),
  nl: () => import("../i18n/nl.json"),
  pl: () => import("../i18n/pl.json"),
  "pt-BR": () => import("../i18n/pt-BR.json"),
  "pt-PT": () => import("../i18n/pt-PT.json"),
  ro: () => import("../i18n/ro.json"),
  ru: () => import("../i18n/ru.json"),
  sv: () => import("../i18n/sv.json"),
  tr: () => import("../i18n/tr.json"),
  uk: () => import("../i18n/uk.json"),
  "zh-Hans": () => import("../i18n/zh-Hans.json"),
  "zh-Hant": () => import("../i18n/zh-Hant.json"),
};

const storageKey = "orbit.language.v1";
let current: Language = "en";
let table: Catalogue = {};
let plurals = new Intl.PluralRules("en");
let numbers = new Intl.NumberFormat("en");

function known(code: string | null | undefined): Language | undefined {
  return languages.find((language) => language.code === code)?.code;
}

// The closest supported language for a browser tag: de-AT is German,
// Portuguese is Brazilian unless it names another country, Chinese is
// Traditional for Taiwan, Hong Kong and Macau, and any Norwegian is Bokmål.
export function matchLanguage(tag: string): Language | undefined {
  const parts = tag.toLowerCase().replace(/_/g, "-").split("-");
  const [base] = parts;
  if (base === "pt") return parts.length === 1 || parts.includes("br") ? "pt-BR" : "pt-PT";
  if (base === "zh")
    return parts.some((part) => ["hant", "tw", "hk", "mo"].includes(part)) ? "zh-Hant" : "zh-Hans";
  if (base === "no" || base === "nn") return "nb";
  return known(base);
}

// Right to left: the page and its layout mirror.
export function rightToLeft(code: Language = current) {
  return code === "ar";
}

// Arabic keeps Western digits, as the TV app draws them.
function locale(code: Language) {
  return code === "ar" ? "ar-u-nu-latn" : code;
}

// The language chosen in App settings on this device, or "" to follow the browser.
export function savedLanguage(): Language | "" {
  try {
    return known(localStorage.getItem(storageKey)) || "";
  } catch {
    return "";
  }
}

// The browser's own preference, ignoring any choice saved in Orbit.
export function browserLanguage(): Language {
  for (const tag of navigator.languages?.length
    ? navigator.languages
    : [navigator.language]) {
    const match = tag && matchLanguage(tag);
    if (match) return match;
  }
  return "en";
}

export function detectLanguage(): Language {
  return savedLanguage() || browserLanguage();
}

// Switches to a catalogue already in hand; loadLanguage fetches it first.
export function applyCatalogue(code: Language, catalogue: Catalogue) {
  table = catalogue;
  current = code;
  plurals = new Intl.PluralRules(code);
  numbers = new Intl.NumberFormat(locale(code));
}

// Called once before the first render.
export async function loadLanguage(code: Language = detectLanguage()) {
  try {
    applyCatalogue(
      code,
      code === "en" ? {} : (await catalogues[code]()).default,
    );
  } catch {
    // A catalogue that cannot load leaves the interface in English.
    applyCatalogue("en", {});
  }
  document.documentElement.lang = current;
  document.documentElement.dir = rightToLeft() ? "rtl" : "ltr";
}

export function language(): Language {
  return current;
}

// Saves the choice ("" follows the browser) and reloads into it.
export function setLanguage(code: Language | "") {
  try {
    if (code) localStorage.setItem(storageKey, code);
    else localStorage.removeItem(storageKey);
  } catch {
    /* Without storage the choice lasts until the page closes. */
  }
  void loadLanguage(code || detectLanguage()).then(() =>
    window.location.reload(),
  );
}

function fill(text: string, vars?: Vars) {
  if (!vars) return text;
  return text.replace(/\{(\w+)\}/g, (match, name: string) =>
    name in vars
      ? typeof vars[name] === "number"
        ? numbers.format(vars[name] as number)
        : String(vars[name])
      : match,
  );
}

// t("Download to PS5"); t("Saves to {path}", { path }).
export function t(text: string, vars?: Vars): string {
  const entry = table[text];
  return fill(typeof entry === "string" ? entry : text, vars);
}

// tn(count, "{count} game", "{count} games"): {count} is filled in.
export function tn(count: number, one: string, other: string, vars?: Vars) {
  const entry = table[other];
  const all = { ...vars, count };
  if (entry && typeof entry === "object")
    return fill(entry[plurals.select(count)] ?? entry.other ?? other, all);
  return fill(count === 1 ? one : other, all);
}

export function formatNumber(
  value: number,
  options?: Intl.NumberFormatOptions,
) {
  return options
    ? new Intl.NumberFormat(locale(current), options).format(value)
    : numbers.format(value);
}

export function formatDate(
  value: Date | number,
  options?: Intl.DateTimeFormatOptions,
) {
  return new Intl.DateTimeFormat(locale(current), options).format(value);
}

// Catalogue genres combine names: "Adventure / Action".
export function genreName(genre: string) {
  return genre
    .split(/ \/ |,/) // as browse.ts GENRE_SEPARATOR
    .map((part) => t(part.trim()))
    .join(" / ");
}

// Drive and location names from Orbit and Library, some numbered.
export function placeName(name: string) {
  const usb = /^USB storage (\d+)$/.exec(name) || /^USB (\d+)$/.exec(name);
  if (usb) return t("USB storage {number}", { number: usb[1] });
  const extended = /^Extended storage (\d+)$/.exec(name);
  if (extended) return t("Extended storage {number}", { number: extended[1] });
  return t(name);
}
