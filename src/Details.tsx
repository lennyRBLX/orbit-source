import { Artwork } from "./Artwork";
import { GameInsights } from "./GameInsights";
import { AddonInfo } from "./AddonInfo";
import { api } from "./api";
import { useEffect, useRef, useState } from "react";
import { Icon } from "./components";
import { openFreeSpace } from "./FreeSpace";
import { GameSheet, PadSymbol, type SheetView } from "./GameSheet";
import { useDebrid, torboxAvailable } from "./Debrid";
import {
  bytes,
  formatLabel,
  formatVersion,
  type Release,
  releaseProvider,
  releaseSize,
  releaseVersion,
  type Game,
  type BrowserSession,
  type Job,
  type Storage,
  type System,
  type LibraryGame,
  type LibraryTarget,
} from "./types";
import { defaultDrive, downloadSpacePlan, releaseState } from "./collection";
import { gamePlatformLabel } from "./platform";
import type { BrowserSelection } from "./browserHandoff";
import { formatDate, genreName, placeName, t } from "./i18n";
import { firmwareLabel, firmwareWarning } from "./firmware";
import { rich } from "./Rich";
import "./details.css";
import { useInterfacePrefs } from "./interfaces/choice";
import { CinemaStage } from "./interfaces/CinemaParts";
import { CrossbarList, type XbRow } from "./interfaces/CrossbarList";
import { Glyph } from "./interfaces/Glyph";
import { PnGroup, PnTile } from "./interfaces/PanoramaParts";
import { menuKeys } from "./interfaces/menuKeys";
import type { BrowseFilters } from "./browse";
import { preferredRelease, rememberChoice } from "./downloadChoice";
import {
  CinemaEverything,
  CrossbarEverything,
  PanoramaEverything,
  StoreEverything,
  useEverything,
  type BundleCatalog,
} from "./GetEverything";
const noCatalog: BundleCatalog = {
  addons: [],
  bundle: null,
  startBundle: () => Promise.reject(new Error("")),
  cancelBundle: () => Promise.reject(new Error("")),
  watchBundle: () => () => undefined,
};
export function Details({
  addonCount = 0, openAddons,
  initialSelection,
  browseFilters,
  browseTorbox,
  back,
  game,
  drives,
  system,
  jobs,
  pair,
  download,
  openQueue,
  browser,
  openProvider,
  fetchPspkgLinks,
  downloadPspkgLink,
  favorite,
  setFavorite,
  configureDebrid,
  library,
  openLibrary,
  section,
  catalog = noCatalog,
}: {
  // Add-ons and Download all, for Get everything.
  catalog?: BundleCatalog;
  addonCount?: number;
  openAddons?: () => void;
  initialSelection?: BrowserSelection;
  // The filters Browse had when the game was opened: its matching option comes first.
  browseFilters?: BrowseFilters;
  // The sources TorBox can download now, for Browse's "TorBox available" source.
  browseTorbox?: ReadonlySet<string>;
  back: () => void;
  game: Game;
  drives: Storage[];
  system: System | null;
  jobs: Job[];
  pair: () => void;
  download: (
    id: string,
    storage: string,
    delivery?: "direct" | "torbox",
  ) => Promise<void>;
  openQueue: (id?: string) => void;
  browser: BrowserSession | null;
  openProvider: (releaseId: string, storageId: string) => Promise<void>;
  fetchPspkgLinks: (releaseId: string, gameId: string) => Promise<void>;
  downloadPspkgLink: (releaseId: string, linkId: string, storageId: string, delivery: "direct" | "torbox") => Promise<void>;
  favorite: boolean;
  setFavorite: (value: boolean) => Promise<void>;
  configureDebrid: (onReturn: () => void) => void;
  library: LibraryGame[];
  openLibrary: (target: Omit<LibraryTarget, "request">) => void;
  // The section the game was opened from, for Crossbar's breadcrumb.
  section?: string;
}) {
  // Only a drive the user picked (here or before a browser handoff). Until
  // then the default follows the selected option's size.
  const [destination, setDestination] = useState(
      initialSelection?.storageId || "",
    ),
    [releaseId, setReleaseId] = useState(
      () => initialSelection?.releaseId || preferredRelease(game.releases, browseFilters, undefined, browseTorbox).id,
    ),
    [pending, setPending] = useState(false),
    [error, setError] = useState("");
  const [downloadAgain, setDownloadAgain] = useState(false);
  const [sheet, setSheet] = useState<SheetView | null>(
    initialSelection ? "downloads" : null,
  );
  const page = useRef<HTMLElement>(null);
  const tagline = game.tagline?.trim();
  const description = game.description?.trim();
  const addonAccess = !game.addonId && addonCount > 0 && <button className="addon-access" onClick={openAddons}>{t("Add-ons ({count})", { count: addonCount })}</button>;
  const debrid = useDebrid(!!system?.paired);
  const [deliveryChoice, setDelivery] = useState<"direct" | "torbox" | null>(null);
  const iface = useInterfacePrefs().prefs.name;
  // Cinema shows About in place of the download choices.
  const [showAbout, setShowAbout] = useState(false);
  // Crossbar: the focused row and the choice menu that's open.
  const [xbRow, setXbRow] = useState<string | undefined>();
  const [xbMenu, setXbMenu] = useState<"version" | "using" | "save" | "everything" | null>(null);
  // Cinema: the sheet takes the focus as it rises (its start button, the
  // chosen version, delivery and drive beside it; or the facts), and hands
  // it back to the button that raised it, as on the PS5 app.
  const cinemaSheet = iface === "cinema" ? sheet : null;
  const lastSheet = useRef<SheetView | null>(null);
  useEffect(() => {
    if (iface !== "cinema") return;
    const box = page.current;
    if (cinemaSheet === "downloads" || cinemaSheet === "about" || cinemaSheet === "everything") {
      lastSheet.current = cinemaSheet;
      requestAnimationFrame(() =>
        (
          box?.querySelector<HTMLElement>(".cn-sheet .cn-sheet-left .cn-button.primary:not([disabled])") ||
          box?.querySelector<HTMLElement>('.cn-sheet .cn-choice[aria-pressed="true"]') ||
          box?.querySelector<HTMLElement>(".cn-sheet .cn-about") ||
          box?.querySelector<HTMLElement>(".cn-sheet button:not([hidden]):not([disabled])")
        )?.focus({ preventScroll: true }),
      );
    } else if (lastSheet.current) {
      const from = lastSheet.current;
      lastSheet.current = null;
      requestAnimationFrame(() =>
        box
          ?.querySelector<HTMLElement>(from === "about" ? ".cn-about-button" : from === "everything" ? ".cn-everything-button" : ".hub-primary")
          ?.focus({ preventScroll: true }),
      );
    }
  }, [iface, cinemaSheet]);
  // A choice menu closing hands the focus back to the row that opened it.
  useEffect(() => {
    if (xbMenu === null && (document.activeElement === document.body || !document.activeElement))
      document.querySelector<HTMLElement>(".xb-game .xb-row.on .xb-row-main")?.focus({ preventScroll: true });
  }, [xbMenu]);
  type PspkgListing = { id: string; title: string; titleIdMatch: boolean };
  type CommunityTitle = { releases: Release[]; pspkg: PspkgListing[]; pspkgIndexed: boolean; pspkgPending: boolean };
  const [community, setCommunity] = useState<CommunityTitle>();
  const [pspkgListing, setPspkgListing] = useState("");
  const [pspkgDelivery, setPspkgDelivery] = useState<"torbox" | "direct">("torbox");
  const anchorId = game.releases[0]?.id;
  useEffect(() => {
    if (!system?.paired || !anchorId || game.addonId) return;
    let current = true;
    let timer: ReturnType<typeof setTimeout> | undefined;
    async function fetchCommunityTitle() {
      try {
        const result = await api<CommunityTitle>(`/community-title?release=${encodeURIComponent(anchorId!)}`);
        if (!current) return;
        setCommunity(result);
        if (result.pspkgPending) timer = setTimeout(fetchCommunityTitle, 1500);
      } catch {
        // Keep the catalogue options already available for this title.
      }
    }
    void fetchCommunityTitle();
    return () => { current = false; clearTimeout(timer); };
  }, [anchorId, game.addonId, system?.paired, system?.remoteGeneration]);
  const communityOptions = [
    ...game.releases.map((r) => community?.releases.find((extra) => extra.id === r.id) ?? r),
    ...(community?.releases.filter((r) => !game.releases.some((existing) => existing.id === r.id)) ?? []),
  ];
  type ProviderFile = {
    id: string;
    status: string;
    sizeOnly?: boolean;
    mirrorSizeConflict?: boolean;
    sizeBytes: number;
    exact: boolean;
    mirrorCount?: number;
  } &
    Partial<Pick<Release, "filename" | "filenameHint" | "resolvedVersion" | "format" | "compression" | "provider" |
      "torboxHosts" | "torboxCached" | "torboxSizeBytes" | "torboxPending" | "browserAvailable" | "directAvailable" | "sizeEstimateBytes">>;
  const [providerFiles, setProviderFiles] = useState<Record<string, ProviderFile>>({});
  const torboxRevision = debrid.status?.connected && !debrid.status.busy ? debrid.status.revision : undefined;
  const metadataIds = communityOptions
    .filter((r) => r.providerMetadata)
    .map((r) => r.id)
    .join(",");
  useEffect(() => {
    setProviderFiles({});
    if (!system?.paired || !metadataIds) return;
    const ids = metadataIds.split(",");
    let cancelled = false, timer: ReturnType<typeof setTimeout> | undefined;
    let offset = 0;
    async function poll() {
      const batch = ids.slice(offset, offset + 32).join(",");
      try {
        const info = await api<{ pending: boolean; files: ProviderFile[] }>(
          `/community-metadata?ids=${encodeURIComponent(batch)}`,
        );
        if (cancelled) return;
        setProviderFiles((current) => ({
          ...current,
          ...Object.fromEntries(info.files.map((file) => [file.id, file])),
        }));
        if (!info.pending) offset += 32;
      } catch {
        // Retry the current batch while this title remains open.
      }
      if (!cancelled && offset < ids.length) timer = setTimeout(poll, 1200);
    }
    void poll();
    return () => { cancelled = true; clearTimeout(timer); };
  }, [metadataIds, system?.paired, torboxRevision]);
  const options = communityOptions.map((r) => {
    const file = providerFiles[r.id];
    if (!file) return r;
    return {
      ...r,
      providerMissing: file.status === "missing" || file.status === "unsupported-format" ||
        file.status === "archive-login-required" ||
        (!!r.providerMissing && file.status === "fetching"),
      provider: file.provider || r.provider,
      browserAvailable: file.browserAvailable ?? r.browserAvailable,
      directAvailable: file.directAvailable ?? r.directAvailable,
      torboxHost: file.torboxHosts ? undefined : r.torboxHost,
      torboxHosts: file.torboxHosts ?? r.torboxHosts,
      torboxCached: file.torboxCached,
      torboxSizeBytes: file.torboxSizeBytes,
      torboxPending: file.torboxPending,
      mirrorCount: file.mirrorCount ?? r.mirrorCount,
      filename: file.provider ? file.filename || "" : file.filename || r.filename,
      filenameHint: file.provider ? file.filenameHint : r.filenameHint,
      resolvedVersion: file.provider || file.filename ? file.resolvedVersion : r.resolvedVersion,
      format: file.format || r.format,
      compression: file.provider ? file.compression : r.compression,
      sizeEstimateBytes: file.sizeBytes || (file.provider ? file.sizeEstimateBytes : r.sizeEstimateBytes),
      sizeEstimateExact: file.sizeBytes ? file.exact : !file.provider && r.sizeEstimateExact,
      metadataStatus: file.mirrorSizeConflict ? "ambiguous-size" : file.status,
      metadataSizeOnly: file.sizeOnly,
    };
  });
  const releases = options.filter((r) => !r.providerMissing ||
    (r.torboxCached && torboxAvailable(debrid.status, r)));
  const cacheLabel = (option: Release, prefix = "") =>
    option.torboxCached && torboxAvailable(debrid.status, option) ? prefix + t("Cached") : "";
  // Preserve the game and its library actions when every download option is missing.
  const release = releases.find((r) => r.id === releaseId) || releases[0] ||
    options.find((r) => r.id === releaseId) || options[0];
  const isPspkg = release.sourceId === "remote" && release.provider === "PSPKG";
  const version = releaseVersion(release);
  const pspkgHere = browser?.pspkgGameId === game.id || browser?.pspkgReleaseId === release.id ? browser : null;
  const listings = community?.pspkg.length ? community.pspkg : isPspkg ? [{ id: release.id, title: release.title, titleIdMatch: false }] : [];
  const listingId = pspkgListing || pspkgHere?.pspkgReleaseId ||
    listings.find((listing) => listing.titleIdMatch)?.id || listings[0]?.id;
  const pspkgLinks = pspkgHere?.pspkgReleaseId === listingId ? pspkgHere?.pspkgLinks || [] : [];
  const canTorbox = torboxAvailable(debrid.status, release) && (!release.providerMissing || !!release.torboxCached);
  const delivery = canTorbox && (release.providerMissing || deliveryChoice !== "direct") ? "torbox" : "direct";
  const torbox = !isPspkg && delivery === "torbox";
  const displaySize = (option: Release) => releaseSize(option,
    option.id === release.id ? torbox : torboxAvailable(debrid.status, option));
  const transferRelease = torbox && release.torboxSizeBytes
    ? { ...release, sizeBytes: release.torboxSizeBytes } : release;
  const drive = destination
    ? drives.find((d) => d.id === destination)
    : defaultDrive(drives, system?.preferredStorage, transferRelease, jobs);
  // Jobs are per option and drive. A finished one can be downloaded again once its file is gone.
  const existing = jobs.find((job) => job.releaseId === release.id && job.storageId === drive?.id);
  const space = drive ? downloadSpacePlan(transferRelease, drive, jobs) : null;
  const planned = space?.planned;
  const afterDownload = space?.after ?? 0;
  const enough = space?.enough;
  const useBrowser = isPspkg || (!!release.browserAvailable && !release.providerMissing && !torbox);
  const resolveCommunityOption = useBrowser && release.sourceId === "remote";
  // Get everything: this option with its DLC, on the drive Download uses.
  const everything = useEverything({ game, release, delivery, drive, jobs, library, system, catalog });
  const closeEverything = () => {
    everything.dismiss();
    setSheet(null);
  };
  const collection = releaseState(release, jobs, library);
  const libraryGame = collection?.libraryGame;
  // Another format or version of this game is already installed or on a drive.
  const related = collection?.kind === "related" && !!libraryGame;
  const relatedNote = libraryGame?.installed
    ? t("Already installed in a different format or version. Download only if you need this option.")
    : t("Already on one of your drives in a different format or version. Download only if you need this option.");
  // Store's game page says why its button reads Download anyway: where that copy is.
  const relatedStatus = libraryGame
    ? [libraryGame.installed ? t("Installed on PS5") : t("On drive"), placeName(libraryGame.location),
       libraryGame.format !== "Unknown" ? libraryGame.format : ""].filter(Boolean).join(" · ")
    : "";
  const showDownload =
    downloadAgain || !collection || collection.kind === "related";
  const downloadUnavailable =
    (release.providerMissing && !canTorbox) ||
    !communityOptions.some((option) => option.id === releaseId) ||
    pending ||
    !!browser?.active ||
    (!drive && !isPspkg) ||
    !(
      system?.platform === "ps5" ||
      (system?.platform === "fixture" && release.id === "ppsa04029-ffpfsc")
    );
  // Download doesn't fit on this drive: it opens Not enough space instead.
  const short = !!drive && !enough;
  useEffect(() => {
    if (releases.length && release.id !== releaseId)
      setReleaseId(release.id);
  }, [releases.length, release.id, releaseId]);
  useEffect(() => {
    setError("");
    setDownloadAgain(false);
    setDelivery(null);
  }, [releaseId]);
  useEffect(() => {
    window.scrollTo(0, 0);
    if (initialSelection) return;
    page.current
      ?.querySelector<HTMLElement>(".hub-primary")
      ?.focus({ preventScroll: true });
  }, [game.id, initialSelection]);
  // Not enough space: free some up on this drive, or use another drive with room.
  // Either way the download carries on by itself.
  function freeSpace(run: (driveId?: string) => void) {
    if (!drive) return;
    openFreeSpace({
      storageId: drive.id,
      need: { sizeBytes: transferRelease.sizeBytes, bytes: (planned ? 0 : transferRelease.sizeBytes) + 16 * 1024 ** 2 },
      then: { kind: "download", run: () => run() },
      others: drives.filter((d) => d.id !== drive.id && downloadSpacePlan(transferRelease, d, jobs).enough),
      useDrive: (id) => { setDestination(id); run(id); },
    });
  }
  function startDirect() {
    if (short) freeSpace((id) => void start(id));
    else void start();
  }
  async function start(driveId = drive?.id) {
    setPending(true);
    setError("");
    try {
      if (!driveId || (release.providerMissing && !canTorbox)) return;
      await download(release.id, driveId, delivery);
      openQueue();
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setPending(false);
    }
  }
  async function verifyProvider(driveId = drive?.id) {
    if (!driveId) return;
    setPending(true);
    setError("");
    try {
      await openProvider(release.id, driveId);
      setSheet(null);
      requestAnimationFrame(() => {
        document
          .querySelector<HTMLButtonElement>(
            ".provider-browser-notice button:not([disabled])",
          )
          ?.focus();
      });
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setPending(false);
    }
  }
  async function fetchLinks(id = listingId) {
    if (!id) return;
    setPending(true);
    setError("");
    try {
      await fetchPspkgLinks(id, game.id);
      setSheet(null);
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setPending(false);
    }
  }
  async function selectPspkgLink(linkId: string) {
    if (!drive || !listingId) {
      setError(t("Connect a writable drive to your PS5."));
      return;
    }
    setPending(true);
    setError("");
    try {
      await downloadPspkgLink(listingId, linkId, drive.id, pspkgDelivery);
      setSheet(null);
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setPending(false);
    }
  }
  function chooseRelease(id: string) {
    setReleaseId(id);
    setSheet("downloads");
  }
  function toggleFavorite() {
    if (game.addonId) return;
    if (!system?.paired) {
      pair();
      return;
    }
    setPending(true);
    setError("");
    void setFavorite(!favorite)
      .catch((e: Error) => setError(e.message))
      .finally(() => setPending(false));
  }
  function viewLibrary() {
    if (libraryGame)
      openLibrary({
        titleId: libraryGame.titleId,
        path: libraryGame.path,
        title: libraryGame.title,
      });
  }
  function primaryAction(review = false) {
    if (!system?.paired) {
      setSheet(null);
      pair();
    } else if (review) setSheet("downloads");
    else if (!downloadAgain && collection?.job) openQueue(collection.job.id);
    else if (!downloadAgain && collection?.kind === "library" && libraryGame)
      viewLibrary();
    else if (isPspkg) void fetchLinks();
    else if (!release.providerMissing || canTorbox) {
      rememberChoice(release);
      const run = (id?: string) => void (useBrowser && delivery === "direct" ? verifyProvider(id) : start(id));
      if (short) freeSpace(run);
      else run();
    }
  }
  const actionLabel = !system?.paired
    ? t("Pair with your PS5")
    : !downloadAgain && collection?.job
      ? t("View download")
      : !downloadAgain && collection?.kind === "library" && libraryGame
        ? t("View in Library")
        : release.providerMissing && !canTorbox
          ? release.torboxPending ? t("Please wait…") : t("Unavailable")
          : pending
          ? t("Please wait…")
          : isPspkg
            ? pspkgLinks.length ? t("Refresh PSPKG links") : t("Fetch PSPKG links")
          : related && !downloadAgain
            ? t("Download anyway")
            : torbox
            ? t("Download via TorBox")
            : useBrowser
              ? release.sourceId === "vikingfile" || resolveCommunityOption ? t("Download to PS5") : t("Open download page on PS5")
              : existing || downloadAgain
                ? t("Download again")
                : t("Download to PS5");
  const methodLabel = isPspkg
    ? t("Automatic: TorBox or direct")
    : torbox
    ? `TorBox${cacheLabel(release, " · ")}`
    : t("{provider} directly", { provider: releaseProvider(release) });
  const methodNote = isPspkg
    ? t("Orbit checks the selected provider and uses TorBox when supported, otherwise a verified direct file.")
    : !canTorbox
    ? t("TorBox is unavailable for this option. Choose the original source or check App settings.")
    : torbox
      ? t("Uses your TorBox account. Preparation may take time; follow it in Downloads.")
      : t("TorBox is available for this option.");
  const pspkgStatus = (status?: string) => {
    switch (status) {
      case "missing-page": return t("The PSPKG download page is missing.");
      case "verify-required": return t("Complete PSPKG verification in the PS5 browser.");
      case "no-anchors": return t("PSPKG verification completed without usable download links.");
      case "rejected": return t("PSPKG rejected by download policy.");
      case "expired": return t("Fetch this game's PSPKG links again.");
      case "cancelled": return t("Verification cancelled");
      case "partial": return t("Some PSPKG links are ready.");
      case "resolving": return t("Checking the PSPKG provider links.");
      default: return "";
    }
  };
  const reasonLabel = (reason: string) => {
    switch (reason) {
      case "verify-required": return t("Verification required");
      case "unsupported-format": return t("Unsupported download format");
      case "unsupported-category": return t("Unsupported download category");
      case "multipart": return t("Multipart download");
      case "unsafe-url": return t("URL rejected by download policy");
      case "resolving": return t("Checking provider");
      case "missing-page": return t("Provider file missing");
      default: return reason ? t("Provider unavailable") : "";
    }
  };
  const pspkgPanel = !game.addonId && (listings.length > 0 || community?.pspkgIndexed) && (
    <section className="pspkg-panel" aria-label={t("PSPKG download links")}>
      {listings.length === 0 ? <p>{t("No matching PSPKG listing found.")}</p> : (
        <>
          {listings.length > 1 && (
            <select aria-label={t("PSPKG listing")} value={listingId}
              onChange={(event) => setPspkgListing(event.target.value)}>
              {listings.map((listing) => <option key={listing.id} value={listing.id}>{listing.title}{listing.titleIdMatch ? ` · ${t("Matching title ID")}` : ""}</option>)}
            </select>
          )}
          <button className="pspkg-fetch" type="button" disabled={pending || !!browser?.active || !browser?.available}
            onClick={() => void fetchLinks()}>
            {t("Fetch PSPKG")}
          </button>
          {pspkgHere?.pspkgReleaseId === listingId && pspkgStatus(pspkgHere?.pspkgStatus) && (
            <p role="status">{pspkgStatus(pspkgHere?.pspkgStatus)}</p>
          )}
          {pspkgLinks.some((link) => link.torboxAvailable) && (
            <select aria-label={t("Download using")} value={pspkgDelivery}
              onChange={(event) => setPspkgDelivery(event.target.value === "torbox" ? "torbox" : "direct")}>
              <option value="direct">{t("Direct")}</option>
              <option value="torbox">TorBox</option>
            </select>
          )}
          {pspkgLinks.length > 0 && (
            <div className="pspkg-link-list">
              {pspkgLinks.map((link) => {
                const size = pspkgDelivery === "torbox" && link.torboxAvailable && link.torboxSizeBytes
                  ? bytes(link.torboxSizeBytes)
                  : link.sizeBytes ? `${link.sizeExact ? "" : "≈"}${bytes(link.sizeBytes)}` : "";
                return (
                  <button key={link.id} type="button"
                    disabled={pending || !!browser?.active || !drive || (!!link.reason && link.reason !== "verify-required")}
                    onClick={() => void selectPspkgLink(link.id)}>
                    <strong>{link.label === "PSPKG" && !link.edition ? `PSPKG ${link.number}` : link.label}</strong>
                    {link.gameName && <small>{[link.gameName, link.titleId, link.region].filter(Boolean).join(" · ")}</small>}
                    <small>{[link.providerHost, link.edition || [link.titleId, link.region, link.format, link.compression].filter(Boolean).join(" · "),
                      link.group, link.torboxCached && t("Cached"), reasonLabel(link.reason)].filter(Boolean).join(" · ")}</small>
                    {(link.version || link.filename || size || link.hasArchivePassword) ? (
                      <small>{[formatVersion(link.version), link.filename, size,
                        link.hasArchivePassword && t("Archive password")].filter(Boolean).join(" · ")}</small>
                    ) : null}
                  </button>
                );
              })}
            </div>
          )}
        </>
      )}
    </section>
  );
  const instructions = (
    <div
      className="provider-browser-option"
      id="provider-browser-steps"
      tabIndex={0}
      data-controller-scroll
      data-initial-focus
      role="region"
      aria-label={t("{provider}: start here", { provider: releaseProvider(release) })}
    >
      <strong>{t("{provider}: start here", { provider: releaseProvider(release) })}</strong>
      <ol>
        <li>
          {rich("First, choose your drive and select {button}.", {
            button: (
              <strong>
                {related && !downloadAgain
                  ? t("Download anyway")
                  : release.sourceId === "vikingfile" || resolveCommunityOption
                    ? t("Download to PS5")
                    : t("Open download page on PS5")}
              </strong>
            ),
          })}
        </li>
        <li>
          {resolveCommunityOption ? t("If a provider page opens, complete verification and select Download when prompted.") :
          release.sourceId === "akirabox" || release.sourceId === "vikingfile" ? t("Complete verification if shown, then wait. Orbit will check the generated download link automatically.") :
          release.sourceId === "datanodes" ? t("Scroll to the bottom, select Continue to Download, then Free Download. Wait for the countdown and select Start Download.") :
          rich(
            "On {provider}, complete any verification and select the site’s {button} button.",
            { provider: releaseProvider(release), button: <strong>{t("Download")}</strong> },
          )}
        </li>
        <li>
          {release.sourceId === "akirabox" || release.sourceId === "vikingfile" ? t("Orbit returns automatically when the file is ready. Follow progress in Downloads.") : rich(
            "Return to {app}. Open {downloads} to follow progress once Orbit has checked the file.",
            {
              app: <strong>Orbit Store</strong>,
              downloads: <strong>{t("Downloads")}</strong>,
            },
          )}
        </li>
      </ol>
      {release.sourceId === "vikingfile" && <p>{t("Orbit uses your Vikingfile sign-in when requesting download links. If verification is needed, its page opens.")}</p>}
      {!browser?.available && (
        <p>{t("Connect to Orbit running on your PS5 to use this option.")}</p>
      )}
    </div>
  );
  const storageBudget = drive && (
    <dl className="sheet-budget" aria-label={t("Download storage estimate")}>
      <div>
        <dt>{t("Free now")}</dt>
        <dd>{bytes(drive.freeBytes)}</dd>
      </div>
      <div>
        <dt>{t("Unfinished downloads")}</dt>
        <dd>{bytes(drive.pendingBytes)}</dd>
      </div>
      <div>
        <dt>
          {planned ? t("After your queue") : t("After queue + this download")}
        </dt>
        <dd>
          {afterDownload < 0
            ? t("{size} short", { size: bytes(-afterDownload) })
            : bytes(afterDownload)}
        </dd>
      </div>
    </dl>
  );
  const downloadSettings = (
    <div className="sheet-settings">
      <button
        className="sheet-setting"
        data-setting="source"
        data-initial-focus
        disabled={pending || browser?.active}
        onClick={() => setSheet("source")}
      >
        <span className="sheet-setting-icon">
          <Icon name="sources" />
        </span>
        <span>
          <small>{t("Source")}</small>
          <strong>
            {releaseProvider(release)} · {formatLabel(release)}
          </strong>
          <small>
            {version && `${t("version {version}", { version })} · `}
            {displaySize(release)}
          </small>
        </span>
        <Icon name="chevron" />
      </button>
      {showDownload && (
        <>
          {!isPspkg && <button
            className="sheet-setting"
            data-setting="delivery"
            disabled={pending || browser?.active}
            onClick={() => setSheet("delivery")}
          >
            <span className="sheet-setting-icon">
              <Icon name="download" />
            </span>
            <span>
              <small>{t("Download using")}</small>
              <strong>{methodLabel}</strong>
              {!torbox && canTorbox && <small>{t("TorBox available")}</small>}
            </span>
            <Icon name="chevron" />
          </button>}
          {system?.paired && (
            <button
              className="sheet-setting"
              data-setting="storage"
              disabled={pending || browser?.active}
              onClick={() => setSheet("storage")}
            >
              <span className="sheet-setting-icon">
                <Icon name="usb" />
              </span>
              <span>
                <small>{t("Save to")}</small>
                <strong>
                  {drive ? placeName(drive.label) : t("Select storage")}
                </strong>
                {drive && (
                  <small>
                    {t("{size} free", {
                      size: bytes(drive.freeBytes),
                    })}
                  </small>
                )}
              </span>
              <Icon name="chevron" />
            </button>
          )}
        </>
      )}
    </div>
  );
  function metadataReason(status: string) {
    switch (status) {
      case "challenge": case "interactive-verification": return t("Verification required");
      case "timeout": return t("Provider request timed out");
      case "fetching": return t("Checking provider");
      case "parse-no-size": return t("Provider did not report a file size");
      case "redirected-to-invalid-host": return t("URL rejected by download policy");
      default: return t("Provider metadata unavailable");
    }
  }
  const facts = (
    <>
      <GameInsights insights={game.insights} />
      {(game.publisher || game.releaseDate) && (
        <dl className="facts game-facts">
          {game.publisher && (
            <div>
              <dt>{t("Publisher")}</dt>
              <dd><bdi>{game.publisher}</bdi></dd>
            </div>
          )}
          {game.releaseDate && (
            <div>
              <dt>{t("Release date")}</dt>
              <dd>
                {formatDate(new Date(game.releaseDate), {
                  dateStyle: "medium",
                  timeZone: "UTC",
                })}
              </dd>
            </div>
          )}
        </dl>
      )}
      <dl className="facts release-facts">
        {(release.format !== "Unknown" || release.compression) && (
          <div>
            <dt>{t("Format")}</dt>
            <dd>{formatLabel(release)}</dd>
          </div>
        )}
        {release.sourceId === "remote" && release.filename && (
          <div>
            <dt>{t("File")}</dt>
            <dd className="release-filename">{release.filename}</dd>
          </div>
        )}
        {!release.filename && release.filenameHint && (
          <div><dt>{t("Filename from source")}</dt><dd>{release.filenameHint}</dd></div>
        )}
        {release.edition && <div><dt>{t("Edition")}</dt><dd>{release.edition}</dd></div>}
        <div>
          <dt>{t("Title ID")}</dt>
          <dd>{release.titleId}</dd>
        </div>
        <div>
          <dt>{t("Source")}</dt>
          <dd>{releaseProvider(release)}</dd>
        </div>
        {version && (
          <div>
            <dt>{t("Version")}</dt>
            <dd>{version}</dd>
          </div>
        )}
        <div>
          <dt>{release.compression ? t("Compressed size") : t("Download size")}</dt>
          <dd>{displaySize(release)}
            {release.metadataStatus === "ambiguous-size" && !(torbox && release.torboxSizeBytes) &&
              <small> · {t("Conflicting reported sizes")}</small>}
            {release.metadataSizeOnly && <small> · {t("Filename unverified")}</small>}
            {!release.sizeBytes && !release.sizeEstimateBytes && !(torbox && release.torboxSizeBytes) && release.metadataStatus &&
              release.metadataStatus !== "ambiguous-size" && <small> · {metadataReason(release.metadataStatus)}</small>}
          </dd>
        </div>
        {release.compression && !!release.extractedBytes && (
          <div>
            <dt>{t("Size after extracting")}</dt>
            <dd>{bytes(release.extractedBytes || 0)}</dd>
          </div>
        )}
      </dl>
    </>
  );
  const primaryDisabled =
    system?.paired && showDownload
      ? downloadUnavailable || (useBrowser && !browser?.available)
      : pending;
  // The notes Store shows around its download button, for Crossbar and Panorama.
  const noteList: { key: string; text: string; warn?: boolean }[] = [
    ...(initialSelection
      ? [{ key: "handoff", text: t("Your game and source are selected from the app. Check the save location, then open the download page below.") }]
      : []),
    ...(!communityOptions.some((option) => option.id === releaseId)
      ? [{ key: "gone", warn: true, text: t("The selected source is no longer available. Choose another download option to continue.") }]
      : []),
    ...(firmwareWarning(release, system) ? [{ key: "firmware", warn: true, text: firmwareWarning(release, system) }] : []),
    ...(system?.paired && related ? [{ key: "related", warn: true, text: relatedNote }] : []),
    ...(destination && !drive && system?.paired
      ? [{ key: "drive-gone", warn: true, text: t("Your selected drive is unavailable. Connect it or choose a save location to continue.") }]
      : []),
    ...(showDownload && release.compression
      ? [{ key: "compressed", text: t("Compressed download. Orbit checks the files and available space before extraction. A password may be required.") }]
      : []),
    ...(showDownload && release.providerMissing && !canTorbox && !release.torboxPending
      ? [{ key: "missing", warn: true, text: t("The source no longer has this file. Choose another download option if there is one.") }]
      : []),
    ...(showDownload && release.notes ? [{ key: "release", text: release.notes }] : []),
    ...(showDownload && (torbox || (debrid.status?.connected && !canTorbox)) ? [{ key: "method", text: methodNote }] : []),
    ...(system?.paired && libraryGame && !related
      ? [{ key: "library", text: t("In your Library on {location}.", { location: placeName(libraryGame.location) }) }]
      : []),
    ...(collection?.job?.status === "complete"
      ? [{ key: "history", text: t("Download history records a completed transfer. Find it in Library to check its current location.") }]
      : []),
    ...(showDownload && system?.paired && system.platform !== "desktop" && !drive
      ? [{ key: "no-drive", warn: true, text: t("Connect a writable drive to your PS5.") }]
      : []),
    ...(error ? [{ key: "error", warn: true, text: error }] : []),
  ];
  // What the main button does, under its label: size and drive, or the handoff.
  const primaryDetail =
    showDownload && drive
      ? t("{size} · to {drive}", { size: displaySize(release), drive: placeName(drive.label) })
      : displaySize(release);
  if (iface === "crossbar") {
    const notes = (
      <>
        {noteList.map((n) => (
          <span key={n.key} className={n.warn ? "xb-note warn" : "xb-note"} role={n.key === "error" ? "alert" : undefined}>
            {n.text}
          </span>
        ))}
      </>
    );
    const valueRow = (key: "version" | "using" | "save", glyph: "layers" | "cloud" | "drive", label: string, value: string, enabled = true): XbRow => ({
      key,
      compact: true,
      disabled: !enabled,
      activate: () => enabled && setXbMenu(key),
      render: () => (
        <>
          <span className="xb-row-icon">
            <Glyph name={glyph} size={22} />
          </span>
          <span className="xb-row-text">
            <span className="xb-row-title">{label}</span>
          </span>
          <span className="xb-row-value">{value}</span>
        </>
      ),
    });
    const rows: XbRow[] = [
      {
        key: "download",
        activate: () => !primaryDisabled && primaryAction(),
        render: (focused: boolean) => (
          <>
            <span className="xb-row-icon xb-primary-icon">
              <Glyph name={useBrowser && showDownload ? "send" : collection?.kind === "library" && !downloadAgain ? "books" : "down"} size={28} />
            </span>
            <span className="xb-row-text">
              <span className="xb-row-title">{actionLabel}</span>
              <span className="xb-row-sub">{primaryDetail}</span>
              {focused && notes}
            </span>
          </>
        ),
        actions: (
          <>
            <button
              className="primary hub-primary"
              disabled={primaryDisabled}
              onClick={() => primaryAction()}
              aria-describedby={showDownload && useBrowser ? "provider-browser-steps" : undefined}
            >
              <Glyph name={useBrowser && showDownload ? "send" : collection?.kind === "library" && !downloadAgain ? "books" : "down"} size={15} stroke={2} />
              {actionLabel}
            </button>
            {collection?.kind === "related" && libraryGame && (
              <button onClick={() => openLibrary({ titleId: libraryGame.titleId, path: libraryGame.path, title: libraryGame.title })}>
                {t("View related copy")}
              </button>
            )}
            {system?.paired && !downloadAgain && (collection?.kind === "library" || collection?.job?.status === "complete") && (
              <button onClick={() => setDownloadAgain(true)}>{t("Download another copy")}</button>
            )}
            {showDownload && system?.paired && !torbox && release.browserAvailable && release.directAvailable !== false && (
              <button onClick={startDirect} disabled={downloadUnavailable}>
                {t("Download directly")}
              </button>
            )}
          </>
        ),
      },
      ...(everything.available
        ? [
            {
              key: "everything",
              compact: true,
              activate: () => setXbMenu("everything"),
              render: () => (
                <>
                  <span className="xb-row-icon">
                    <Glyph name="layers" size={22} />
                  </span>
                  <span className="xb-row-text">
                    <span className="xb-row-title">{t("Get everything")}</span>
                    <span className="xb-row-sub">{everything.offer}</span>
                  </span>
                </>
              ),
            },
          ]
        : []),
      valueRow("version", "layers", t("Version"), `${releaseProvider(release)} · ${formatLabel(release)}`, !pending && !browser?.active),
      ...(showDownload && !isPspkg ? [valueRow("using", "cloud", t("Download using"), methodLabel, !pending && !browser?.active)] : []),
      ...(showDownload && system?.paired
        ? [valueRow("save", "drive", t("Save to"), drive ? placeName(drive.label) : t("Select storage"), !pending && !browser?.active)]
        : []),
      {
        key: "favourite",
        compact: true,
        activate: () => toggleFavorite(),
        disabled: !!game.addonId,
        render: () => (
          <>
            <span className="xb-row-icon">
              <Glyph name={favorite ? "heartFill" : "heart"} size={22} />
            </span>
            <span className="xb-row-text">
              <span className="xb-row-title">{favorite ? t("Saved to favourites") : t("Add to favourites")}</span>
            </span>
          </>
        ),
      },
      {
        key: "about",
        compact: true,
        activate: () => setShowAbout((v) => !v),
        render: () => (
          <>
            <span className="xb-row-icon">
              <Glyph name="info" size={22} />
            </span>
            <span className="xb-row-text">
              <span className="xb-row-title">{t("About this game")}</span>
            </span>
          </>
        ),
      },
    ].filter(row => !game.addonId || row.key !== "favourite");
    const menuItems =
      xbMenu === "version"
        ? releases.map((option) => ({
            key: option.id,
            on: release.id === option.id,
            title: `${releaseProvider(option)} · ${formatLabel(option)}`,
            sub: [
              displaySize(option),
              cacheLabel(option),
              releaseVersion(option) && t("version {version}", { version: releaseVersion(option) }),
              option.region,
              releaseState(option, jobs, library)?.label && t(releaseState(option, jobs, library)!.label),
              firmwareLabel(option),
            ]
              .filter(Boolean)
              .join(" · "),
            choose: () => setReleaseId(option.id),
            disabled: false,
          }))
        : xbMenu === "using"
          ? [
              { key: "direct", on: !torbox, title: t("{provider} directly", { provider: releaseProvider(release) }), sub: "", choose: () => setDelivery("direct"), disabled: !!release.providerMissing },
              !debrid.status?.connected && !torbox
                ? { key: "setup", on: false, title: t("Set up TorBox"), sub: t("Connect your TorBox account in App settings."), choose: () => configureDebrid(() => void debrid.refresh()), disabled: false }
                : { key: "torbox", on: torbox, title: "TorBox", sub: methodNote, choose: () => setDelivery("torbox"), disabled: !canTorbox },
            ]
          : xbMenu === "save"
            ? drives.map((option) => {
                const plan = downloadSpacePlan(release, option, jobs);
                return {
                  key: option.id,
                  on: drive?.id === option.id,
                  title: placeName(option.label),
                  sub: `${t("{free} free of {total}", { free: bytes(option.freeBytes), total: bytes(option.totalBytes) })} · ${plan.after < 0 ? t("{size} short", { size: bytes(-plan.after) }) : t("{size} after queue", { size: bytes(plan.after) })}`,
                  choose: () => setDestination(option.id),
                  disabled: false,
                };
              })
            : [];
    return (
      <section ref={page} className="product native-game xb-game" aria-labelledby="product-title">
        <div className="xb-split">
          <div className="xb-column">
            <p className="xb-breadcrumb">
              <button className="xb-crumb" onClick={back}>
                {section || t("Back")}
              </button>{" "}
              <span aria-hidden="true">›</span> <span id="product-title">{game.addonId && game.baseGameTitle ? `${game.baseGameTitle} · ` : ""}{game.title}</span>
            </p>
            {pspkgPanel}
            <CrossbarList rows={rows} current={xbRow} setCurrent={setXbRow} ariaLabel={game.title} />
            {xbMenu === "everything" ? (
              <CrossbarEverything
                m={everything}
                close={() => {
                  everything.dismiss();
                  setXbMenu(null);
                }}
              />
            ) : xbMenu && (
              <div
                className="xb-menu xb-choice-menu"
                role="menu"
                data-own-arrows
                onKeyDown={(e) => menuKeys(e, () => setXbMenu(null))}
              >
                <span className="xb-menu-title">
                  {xbMenu === "version" ? t("Version") : xbMenu === "using" ? t("Download using") : t("Save to")}
                </span>
                {menuItems.map((item, i) => (
                  <button
                    key={item.key}
                    role="menuitemradio"
                    aria-checked={item.on}
                    autoFocus={item.on || (i === 0 && !menuItems.some((m) => m.on))}
                    disabled={item.disabled}
                    onClick={() => {
                      item.choose();
                      setXbMenu(null);
                    }}
                  >
                    <span className="xb-radio" aria-hidden="true" />
                    <span>
                      <strong><bdi>{item.title}</bdi></strong>
                      {item.sub && <small>{item.sub}</small>}
                    </span>
                  </button>
                ))}
                {xbMenu === "version" && <span className="xb-menu-foot">{t("Available from your enabled sources.")}</span>}
                {xbMenu === "save" && drive && <span className="xb-menu-foot">{t("Saves to {path}", { path: drive.path })}</span>}
              </div>
            )}
          </div>
          <aside className="xb-preview" aria-live="polite">
            {showAbout ? (
              <div className="xb-card xb-about">
                <p className="xb-card-kicker">{t("About this game")}</p>
                {facts}
              </div>
            ) : showDownload && useBrowser && !isPspkg ? (
              // The provider's steps take the art's place, as in the design.
              <div className="xb-card xb-steps">{instructions}</div>
            ) : (
              <>
                <span className="xb-preview-art">
                  <Artwork kind="hero" priority src={game.hero || game.cover} alt="" decoding="async" referrerPolicy="no-referrer" />
                </span>
                {tagline && <h2><bdi>{tagline}</bdi></h2>}
                {description && <p className="xb-preview-text"><bdi>{description}</bdi></p>}
                {addonAccess}
              </>
            )}
            <AddonInfo item={{...game, ...release}} library={library} contents />
            {!showAbout && system?.paired && showDownload && drive && (
              <div className="xb-card xb-estimate">
                <p className="xb-card-kicker">{t("Download storage estimate")}</p>
                {storageBudget}
                <meter
                  className="xb-meter"
                  min={0}
                  max={Math.max(1, drive.totalBytes)}
                  value={Math.max(0, drive.totalBytes - drive.freeBytes)}
                  aria-label={t("Storage used")}
                />
                <p className="xb-card-fine">{t("Estimates include paused and failed downloads. Other apps can change free space.")}</p>
              </div>
            )}
            <p className="xb-card-fine xb-footnote">{t("Saves a file to your console. Installation and launching are separate.")}</p>
          </aside>
        </div>
      </section>
    );
  }
  if (iface === "panorama") {
    const about = () =>
      requestAnimationFrame(() => {
        const group = page.current?.querySelector<HTMLElement>(".pn-about");
        group?.scrollIntoView({ block: "nearest", inline: "start", behavior: "smooth" });
        group?.focus({ preventScroll: true });
      });
    const mainGlyph = useBrowser && showDownload ? "send" : collection?.kind === "library" && !downloadAgain ? "books" : !system?.paired ? "link" : "down";
    return (
      <section ref={page} className="product native-game pn-game" aria-labelledby="product-title">
        <h1 id="product-title" className="sr-title">
          {game.title}
        </h1>
        <div className="pn-page" data-scroll="x">
          {pspkgPanel}
          <PnGroup
            title={t("Download")}
            className="pn-feature pn-game-download"
            foot={
              <>
                {tagline && <p className="pn-game-tagline"><bdi>{tagline}</bdi></p>}
                {description && <p className="pn-game-text"><bdi>{description}</bdi></p>}
                {addonAccess}<AddonInfo item={{...game, ...release}} library={library} contents />
                {noteList.length ? (
                  noteList.map((n) => (
                    <p key={n.key} className={n.warn ? "pn-note warn" : "pn-note"} role={n.key === "error" ? "alert" : undefined}>
                      {n.text}
                    </p>
                  ))
                ) : (
                  <p className="pn-kicker pn-game-meta">
                    {[genreName(game.genre || ""), game.releaseDate?.slice(0, 4), gamePlatformLabel(release), releaseProvider(release)]
                      .filter(Boolean)
                      .join(" · ")}
                  </p>
                )}
                <p className="pn-group-fine">{t("Saves a file to your console. Installation and launching are separate.")}</p>
              </>
            }
          >
            <PnTile size="large" art={game.hero || game.cover} fallback={game.heroFallback || game.cover} />
            <PnTile
              size="wide"
              tone={primaryDisabled ? "dark" : "accent"}
              glyph={mainGlyph}
              title={actionLabel}
              sub={showDownload && useBrowser && initialSelection ? undefined : primaryDetail}
              disabled={primaryDisabled}
              label={actionLabel}
              onClick={() => primaryAction()}
            />
            {!game.addonId && <PnTile
              tone="dark"
              className="lower"
              glyph={favorite ? "heartFill" : "heart"}
              title={favorite ? t("Saved to favourites") : t("Favourite")}
              disabled={pending || !!game.addonId}
              onClick={toggleFavorite}
            />}
            <PnTile tone="dark" className="lower" glyph="info" title={t("About")} label={t("About this game")} onClick={about} />
            {everything.available && (
              <PnTile
                size="wide"
                tone="dark"
                className="lower"
                glyph="layers"
                title={t("Get everything")}
                sub={everything.offer}
                onClick={() =>
                  requestAnimationFrame(() => {
                    const group = page.current?.querySelector<HTMLElement>(".pn-everything");
                    group?.scrollIntoView({ block: "nearest", inline: "start", behavior: "smooth" });
                    group?.querySelector<HTMLElement>(".pn-tile-hit:not([disabled])")?.focus({ preventScroll: true });
                  })
                }
              />
            )}
            {collection?.kind === "related" && libraryGame && (
              <PnTile
                size="wide"
                tone="dark"
                glyph="books"
                title={t("View related copy")}
                onClick={() => openLibrary({ titleId: libraryGame.titleId, path: libraryGame.path, title: libraryGame.title })}
              />
            )}
            {system?.paired && !downloadAgain && (collection?.kind === "library" || collection?.job?.status === "complete") && (
              <PnTile size="wide" tone="dark" glyph="copy" title={t("Download another copy")} onClick={() => setDownloadAgain(true)} />
            )}
            {showDownload && system?.paired && !torbox && release.browserAvailable && release.directAvailable !== false && (
              <PnTile size="wide" tone="dark" glyph="down" title={t("Download directly")} disabled={downloadUnavailable} onClick={startDirect} />
            )}
          </PnGroup>
          {everything.available && <PanoramaEverything m={everything} />}
          {showDownload && !isPspkg && (
            <PnGroup title={t("Download using")}>
              <PnTile
                size="wide"
                tone="dark"
                glyph="cloud"
                title={t("{provider} directly", { provider: releaseProvider(release) })}
                check={!torbox}
                current={!torbox}
                disabled={pending || browser?.active || release.providerMissing}
                onClick={() => setDelivery("direct")}
              />
              {!debrid.status?.connected && !torbox ? (
                <PnTile
                  size="wide"
                  tone="dark"
                  glyph="link"
                  title={t("Set up TorBox")}
                  sub={t("Connect your TorBox account in App settings.")}
                  disabled={pending || browser?.active}
                  onClick={() => configureDebrid(() => void debrid.refresh())}
                />
              ) : (
                <PnTile
                  size="wide"
                  tone="dark"
                  glyph="link"
                  kicker={canTorbox && !torbox ? t("TorBox available") : undefined}
                  title="TorBox"
                  sub={methodNote}
                  check={torbox}
                  current={torbox}
                  disabled={pending || browser?.active || !canTorbox}
                  onClick={() => setDelivery("torbox")}
                />
              )}
            </PnGroup>
          )}
          {showDownload && system?.paired && (
            <PnGroup title={t("Save to")}>
              {drives.length ? (
                drives.map((option) => {
                  const plan = downloadSpacePlan(release, option, jobs);
                  return (
                    <PnTile
                      key={option.id}
                      size="wide"
                      tone="dark"
                      glyph="drive"
                      title={placeName(option.label)}
                      sub={`${t("{size} free", { size: bytes(option.freeBytes) })} · ${
                        plan.after < 0 ? t("{size} short", { size: bytes(-plan.after) }) : t("{size} after queue", { size: bytes(plan.after) })
                      }`}
                      progress={option.totalBytes ? (100 * (option.totalBytes - option.freeBytes)) / option.totalBytes : 0}
                      check={drive?.id === option.id}
                      current={drive?.id === option.id}
                      disabled={pending || browser?.active}
                      onClick={() => setDestination(option.id)}
                    />
                  );
                })
              ) : (
                <div className="pn-empty-card wide">
                  <strong>{t("Connect a writable drive to your PS5.")}</strong>
                </div>
              )}
            </PnGroup>
          )}
          <PnGroup title={t("Versions")} foot={<p className="pn-group-fine">{t("Available from your enabled sources.")}</p>}>
            {releases.map((option) => {
              const presence = releaseState(option, jobs, library);
              return (
                <PnTile
                  key={option.id}
                  size="wide"
                  tone="dark"
                  kicker={releaseProvider(option)}
                  title={`${formatLabel(option)} · ${displaySize(option)}`}
                  sub={[cacheLabel(option), releaseVersion(option) && t("version {version}", { version: releaseVersion(option) }), option.region, presence && t(presence.label), firmwareLabel(option)]
                    .filter(Boolean)
                    .join(" · ")}
                  check={release.id === option.id}
                  current={release.id === option.id}
                  disabled={pending || browser?.active}
                  onClick={() => setReleaseId(option.id)}
                />
              );
            })}
          </PnGroup>
          {system?.paired && showDownload && drive && (
            <section className="pn-group pn-text-group pn-space" aria-label={t("Download storage estimate")}>
              <h2>{t("Space")}</h2>
              {storageBudget}
              <p className="pn-group-fine">
                {t("Download storage estimate")}. {t("Estimates include paused and failed downloads. Other apps can change free space.")}
              </p>
            </section>
          )}
          {showDownload && useBrowser && !isPspkg && <section className="pn-group pn-text-group pn-steps">{instructions}</section>}
          <section className="pn-group pn-text-group pn-about" tabIndex={-1} aria-label={t("About this game")}>
            <h2>{t("About this game")}</h2>
            {facts}
          </section>
        </div>
      </section>
    );
  }
  if (iface === "cinema") {
    // The page: the stage (kicker, title), the line and a few lines about the
    // game, and Download · size, the heart and About. Download raises the
    // sheet with its choices (as on Store's page); About, the facts. The
    // sheet holds the focus until Circle closes it.
    const sheetUp = sheet === "downloads" || sheet === "about" || sheet === "everything";
    const firstAction = !system?.paired
      ? t("Pair with your PS5")
      : showDownload
        ? transferRelease.sizeBytes || release.sizeEstimateBytes
          ? `${t("Download")} · ${displaySize(release)}`
          : t("Download")
        : actionLabel;
    return (
      <section ref={page} className={`product native-game cn-game${sheetUp ? " cn-sheet-up" : ""}`} aria-labelledby="product-title">
        <div className="cn-game-page" inert={sheetUp || undefined}>
          <CinemaStage
            kicker={[game.addonId && game.baseGameTitle, genreName(game.genre || ""), game.releaseDate?.slice(0, 4), gamePlatformLabel(release), release.compression ? formatLabel(release).split(" · ")[0] : release.browserAvailable ? releaseProvider(release) : ""]
              .filter(Boolean)
              .join(" · ")}
            title={game.title}
          />
          <div className="cn-game-intro">
            {pspkgPanel}
            {tagline && <p className="cn-sheet-tagline"><bdi>{tagline}</bdi></p>}
            {description && <p className="cn-body"><bdi>{description}</bdi></p>}
                {addonAccess}<AddonInfo item={{...game, ...release}} library={library} contents />
            {initialSelection && (
              <p className="cn-note">
                {t(
                  "Your game and source are selected from the app. Check the save location, then open the download page below.",
                )}
              </p>
            )}
            {!communityOptions.some((option) => option.id === releaseId) && (
              <p className="cn-note warn" role="alert">
                {t("The selected source is no longer available. Choose another download option to continue.")}
              </p>
            )}
            {firmwareWarning(release, system) && <p className="cn-note warn">{firmwareWarning(release, system)}</p>}
            {destination && !drive && system?.paired && (
              <p className="cn-note warn" role="alert">
                {t("Your selected drive is unavailable. Connect it or choose a save location to continue.")}
              </p>
            )}
            {system?.paired && libraryGame && (
              <p className={related ? "cn-note warn" : "cn-note"}>
                {related
                  ? relatedNote
                  : t("In your Library on {location}.", { location: placeName(libraryGame.location) })}
              </p>
            )}
            {collection?.job?.status === "complete" && (
              <p className="cn-note">
                {t("Download history records a completed transfer. Find it in Library to check its current location.")}
              </p>
            )}
            {error && (
              <p className="cn-note warn" role="alert">
                {error}
              </p>
            )}
            <div className="cn-actions">
              <button
                className="cn-button primary hub-primary"
                disabled={pending}
                onClick={() => (system?.paired && showDownload ? setSheet("downloads") : primaryAction())}
                aria-haspopup={system?.paired && showDownload ? "dialog" : undefined}
              >
                <Glyph name={collection?.kind === "library" && !downloadAgain ? "books" : "down"} size={18} stroke={2} />
                {firstAction}
              </button>
              {everything.available && (
                <button className="cn-button cn-everything-button" aria-haspopup="dialog" onClick={() => setSheet("everything")}>
                  <Glyph name="layers" size={18} />
                  {t("Get everything")}
                </button>
              )}
              {!game.addonId && <button
                className="icon-button iface-favorite"
                data-command="favorite"
                aria-label={favorite ? t("Saved to favourites") : t("Add to favourites")}
                aria-pressed={favorite}
                disabled={pending || !!game.addonId}
                onClick={toggleFavorite}
              >
                <Glyph name={favorite ? "heartFill" : "heart"} />
              </button>}
              <button className="cn-button cn-about-button" data-command="details" aria-haspopup="dialog" onClick={() => setSheet("about")}>
                <Glyph name="info" size={18} />
                {t("About")}
              </button>
            </div>
            <div className="cn-actions">
              {collection?.kind === "related" && libraryGame && (
                <button
                  className="cn-button"
                  onClick={() => openLibrary({ titleId: libraryGame.titleId, path: libraryGame.path, title: libraryGame.title })}
                >
                  {t("View related copy")}
                </button>
              )}
              {system?.paired && !downloadAgain && (collection?.kind === "library" || collection?.job?.status === "complete") && (
                <button className="cn-button" onClick={() => setDownloadAgain(true)}>
                  {t("Download another copy")}
                </button>
              )}
            </div>
          </div>
        </div>
        {sheetUp && (
          <div
            className={`cn-sheet${sheet === "about" ? " cn-sheet-about" : sheet === "everything" ? " cn-sheet-everything" : ""}`}
            role="dialog"
            aria-modal="true"
            aria-label={sheet === "about" ? t("About this game") : sheet === "everything" ? t("Get everything") : t("Download options")}
            onKeyDown={(e) => {
              if (e.key === "Escape" || e.key === "BrowserBack") {
                e.preventDefault();
                if (sheet === "everything") closeEverything();
                else setSheet(null);
              }
            }}
          >
            {/* Circle on a pad closes the sheet, not the page. */}
            <button hidden data-dialog-back onClick={() => (sheet === "everything" ? closeEverything() : setSheet(null))} />
            {sheet === "about" ? (
              <div className="cn-about" tabIndex={0} data-controller-scroll>
                <p className="cn-section-label">{t("About this game")}</p>
                {facts}
              </div>
            ) : sheet === "everything" ? (
              <CinemaEverything m={everything} close={closeEverything} />
            ) : (
              <>
                <div className="cn-sheet-left">
                  {showDownload && useBrowser && !isPspkg && instructions}
                  {showDownload && (torbox || (debrid.status?.connected && !canTorbox)) && <p className="cn-note">{methodNote}</p>}
                  {showDownload && system?.paired && system.platform !== "desktop" && !drive && (
                    <p className="cn-note warn">{t("Connect a writable drive to your PS5.")}</p>
                  )}
                  {error && (
                    <p className="cn-note warn" role="alert">
                      {error}
                    </p>
                  )}
                  <div className="cn-actions">
                    <button
                      className="cn-button primary"
                      disabled={primaryDisabled}
                      onClick={() => primaryAction()}
                      aria-describedby={showDownload && useBrowser ? "provider-browser-steps" : undefined}
                    >
                      <Glyph name={useBrowser && showDownload ? "send" : "down"} size={18} stroke={2} />
                      {actionLabel}
                    </button>
                    {showDownload && system?.paired && !torbox && release.browserAvailable && release.directAvailable !== false && (
                      <button className="cn-button" onClick={startDirect} disabled={downloadUnavailable}>
                        {t("Download directly")}
                      </button>
                    )}
                  </div>
                  <p className="cn-footnote">{t("Saves a file to your console. Installation and launching are separate.")}</p>
                </div>
                <div className="cn-sheet-right">
                  <p className="cn-section-label">{t("Version")}</p>
                  <div className="cn-cards cn-choices">
                    {releases.map((option) => {
                      const presence = releaseState(option, jobs, library);
                      const on = release.id === option.id;
                      return (
                        <button
                          key={option.id}
                          className="cn-choice"
                          aria-pressed={on}
                          disabled={pending || browser?.active}
                          onClick={() => setReleaseId(option.id)}
                        >
                          <small>{releaseProvider(option)}</small>
                          <strong>{formatLabel(option)}</strong>
                          <small>
                            {displaySize(option)}
                            {cacheLabel(option, " · ")}
                            {releaseVersion(option) && ` · ${t("version {version}", { version: releaseVersion(option) })}`}
                            {option.region && ` · ${option.region}`}
                          </small>
                          {presence && <small className="cn-choice-note">{t(presence.label)}</small>}
                          {firmwareLabel(option) && (
                            <small className={option.firmwareIssue ? "cn-choice-note firmware-note warn" : "cn-choice-note firmware-note"}>{firmwareLabel(option)}</small>
                          )}
                          {on && <Glyph name="check" size={16} stroke={2.4} />}
                        </button>
                      );
                    })}
                  </div>
                  {/* What the chosen version says about itself, under Version (as in Orbit Zero). */}
                  {firmwareWarning(release, system) && <p className="cn-note warn">{firmwareWarning(release, system)}</p>}
                  {release.compression && (
                    <p className="cn-note">
                      {t("Compressed download. Orbit checks the files and available space before extraction. A password may be required.")}
                    </p>
                  )}
                  <GameInsights insights={game.insights} />
            <AddonInfo item={release} library={library} />
                  {release.notes && <p className="cn-note">{release.notes}</p>}
                  {showDownload && !isPspkg && (
                    <>
                      <p className="cn-section-label">{t("Download using")}</p>
                      <div className="cn-cards cn-choices">
                        <button
                          className="cn-choice"
                          aria-pressed={!torbox}
                          disabled={pending || browser?.active || release.providerMissing}
                          onClick={() => setDelivery("direct")}
                        >
                          <strong>{t("{provider} directly", { provider: releaseProvider(release) })}</strong>
                          {!torbox && <Glyph name="check" size={16} stroke={2.4} />}
                        </button>
                        {!debrid.status?.connected && !torbox ? (
                          <button
                            className="cn-choice"
                            disabled={pending || browser?.active}
                            onClick={() =>
                              configureDebrid(() => {
                                void debrid.refresh();
                              })
                            }
                          >
                            <strong>{t("Set up TorBox")}</strong>
                            <small>{t("Connect your TorBox account in App settings.")}</small>
                          </button>
                        ) : (
                          <button
                            className="cn-choice"
                            aria-pressed={torbox}
                            disabled={pending || browser?.active || !canTorbox}
                            onClick={() => setDelivery("torbox")}
                          >
                            <strong>TorBox</strong>
                            <small>{methodNote}</small>
                            {torbox && <Glyph name="check" size={16} stroke={2.4} />}
                          </button>
                        )}
                      </div>
                    </>
                  )}
                  {showDownload && system?.paired && (
                    <>
                      <p className="cn-section-label">{t("Save to")}</p>
                      <div className="cn-cards cn-choices">
                        {drives.length ? (
                          drives.map((option) => {
                            const plan = downloadSpacePlan(release, option, jobs);
                            const on = drive?.id === option.id;
                            return (
                              <button
                                key={option.id}
                                className="cn-choice cn-drive"
                                aria-pressed={on}
                                disabled={pending || browser?.active}
                                onClick={() => setDestination(option.id)}
                              >
                                <strong>{placeName(option.label)}</strong>
                                <small>
                                  {t("{size} free", { size: bytes(option.freeBytes) })} ·{" "}
                                  {plan.after < 0
                                    ? t("{size} short", { size: bytes(-plan.after) })
                                    : t("{size} after queue", { size: bytes(plan.after) })}
                                </small>
                                <span className="cn-meter" aria-hidden="true">
                                  <i style={{ width: `${option.totalBytes ? Math.min(100, (100 * (option.totalBytes - option.freeBytes)) / option.totalBytes) : 0}%` }} />
                                </span>
                                <small className="cn-choice-note">{option.path}</small>
                                {on && <Glyph name="check" size={16} stroke={2.4} />}
                              </button>
                            );
                          })
                        ) : (
                          <p className="cn-note">{t("Connect a writable drive to your PS5.")}</p>
                        )}
                      </div>
                      {storageBudget}
                      <p className="cn-footnote">
                        {t("Estimates include paused and failed downloads. Other apps can change free space.")}
                      </p>
                    </>
                  )}
                </div>
              </>
            )}
          </div>
        )}
      </section>
    );
  }
  return (
    <section
      ref={page}
      className="product native-game"
      aria-labelledby="product-title"
    >
      <div className="hub-content" inert={sheet ? true : undefined}>
        <div className="hub-heading">
          {pspkgPanel}
          {game.addonId && game.baseGameTitle && <p className="addon-base-title"><bdi>{game.baseGameTitle}</bdi></p>}
          <h1
            id="product-title"
            className={game.title.length > 34 ? "long-title" : undefined}
          >
            {game.title}
          </h1>
          {tagline && <p className="hub-tagline"><bdi>{tagline}</bdi></p>}
          {description && <p className="hub-description"><bdi>{description}</bdi></p>}
                {addonAccess}<AddonInfo item={{...game, ...release}} library={library} contents />
          <p className="hub-release">
            {releaseProvider(release)}
            <span>·</span>
            {formatLabel(release)}
            <span>·</span>
            {displaySize(release)}
            <span>·</span>
            <span>{gamePlatformLabel(release)}</span>
            {game.genre && (
              <>
                <span>·</span>
                <span>{genreName(game.genre)}</span>
              </>
            )}
            {game.releaseDate && (
              <>
                <span>·</span>
                <time dateTime={game.releaseDate}>{game.releaseDate.slice(0, 4)}</time>
              </>
            )}
          </p>
        </div>
        <div className="hub-launch">
          {system?.paired && related && !downloadAgain && <p className="hub-status">{relatedStatus}</p>}
          <div className="hub-actions">
            <button
              className="primary hub-primary"
              data-long-label={actionLabel.length > 34 || undefined}
              disabled={pending}
              onClick={() => primaryAction(true)}
              aria-haspopup={
                showDownload && system?.paired ? "dialog" : undefined
              }
            >
              <Icon name="download" />
              <span>{actionLabel}</span>
            </button>
            {everything.available && (
              <button className="hub-everything" aria-haspopup="dialog" onClick={() => setSheet("everything")}>
                <Icon name="layers" />
                <span>{t("Get everything")}</span>
              </button>
            )}
            {!game.addonId && <button
              className="hub-round"
              data-command="favorite"
              aria-label={
                favorite ? t("Saved to favourites") : t("Add to favourites")
              }
              aria-pressed={favorite}
              disabled={pending || !!game.addonId}
              onClick={toggleFavorite}
            >
              <Icon name={favorite ? "heartFill" : "heart"} />
            </button>}
            <button
              className="hub-round"
              data-command="details"
              aria-label={t("About this game")}
              aria-haspopup="dialog"
              onClick={() => setSheet("about")}
            >
              <Icon name="more" />
            </button>
          </div>
          {!sheet && error && (
            <p className="form-error" role="alert">
              {error}
            </p>
          )}
        </div>
        <div className="game-pad-hints" aria-label={t("Controls")}>
          <span>
            <PadSymbol name="cross" />
            {t("Select")}
          </span>
          <span>
            <PadSymbol name="circle" />
            {t("Back")}
          </span>
          {!game.addonId && <span>
            <PadSymbol name="triangle" />
            {t("Favourite")}
          </span>}
          <span>
            <PadSymbol name="square" />
            {t("Details")}
          </span>
        </div>
      </div>
      {sheet && (
        <GameSheet
          view={sheet}
          title={
            sheet === "about"
              ? t("About this game")
              : sheet === "everything"
                ? t("Get everything")
                : sheet === "storage"
                ? t("Save to")
                : sheet === "source"
                  ? t("Source")
                  : sheet === "delivery"
                    ? t("Download using")
                    : t("Download options")
          }
          close={() => (sheet === "everything" ? closeEverything() : setSheet(null))}
          back={() =>
            sheet === "everything"
              ? closeEverything()
              : setSheet(
                  sheet === "downloads" || sheet === "about"
                    ? null
                    : "downloads",
                )
          }
        >
          {sheet === "downloads" && downloadSettings}
          {sheet === "everything" && <StoreEverything m={everything} close={closeEverything} />}
          {sheet !== "everything" && <div className="sheet-scroll" data-scroll-region key={sheet}>
            {sheet === "downloads" && firmwareWarning(release, system) && <p className="sheet-note warn">{firmwareWarning(release, system)}</p>}
            {sheet === "downloads" && system?.paired && related && <p className="sheet-note warn">{relatedNote}</p>}
            {sheet === "downloads" && release.compression && <p className="sheet-note">{t("Compressed download. Orbit checks the files and available space before extraction. A password may be required.")}</p>}
            {sheet === "downloads" && <AddonInfo item={release} library={library} />}
            {sheet === "downloads" && release.notes && <p className="sheet-note">{release.notes}</p>}
            {sheet === "about" ? (
              <div
                className="game-about"
                tabIndex={0}
                data-controller-scroll
                data-initial-focus
              >
                <p className="about-title"><bdi>{game.title}</bdi></p>
                {game.description && (
                  <p className="description"><bdi>{game.description}</bdi></p>
                )}
                {facts}
              </div>
            ) : sheet === "source" ? (
              <div className="sheet-choices">
                <p className="sheet-note">
                  {game.releases.length > 1
                    ? t("Choose a source and format for this game.")
                    : t("Available from your enabled sources.")}
                </p>
                {releases.map((option) => {
                  const presence = releaseState(option, jobs, library);
                  return (
                    <button
                      key={option.id}
                      className="sheet-choice release-option"
                      aria-pressed={release.id === option.id}
                      data-initial-focus={release.id === option.id || undefined}
                      disabled={pending || browser?.active}
                      onClick={() => chooseRelease(option.id)}
                    >
                      <span className="sheet-choice-check">
                        {release.id === option.id && <Icon name="check" />}
                      </span>
                      <span>
                        <strong>
                          {releaseProvider(option)} · {formatLabel(option)}
                        </strong>
                        <small>
                          {option.titleId}
                          {releaseVersion(option) &&
                            ` · ${t("version {version}", { version: releaseVersion(option) })}`}
                        </small>
                        <small>
                          {displaySize(option)}
                          {cacheLabel(option, " · ")}
                          {presence && ` · ${t(presence.label)}`}
                        </small>
                        {firmwareLabel(option) && (
                          <small className={option.firmwareIssue ? "firmware-note warn" : "firmware-note"}>{firmwareLabel(option)}</small>
                        )}
                      </span>
                    </button>
                  );
                })}
              </div>
            ) : sheet === "delivery" ? (
              <div className="sheet-choices">
                <button
                  className="sheet-choice delivery-choice"
                  aria-pressed={!torbox}
                  data-delivery="direct"
                  data-initial-focus={!torbox || undefined}
                  disabled={pending || browser?.active || release.providerMissing}
                  onClick={() => {
                    setDelivery("direct");
                    setSheet("downloads");
                  }}
                >
                  <span className="sheet-choice-check">
                    {!torbox && <Icon name="check" />}
                  </span>
                  <strong>
                    {t("{provider} directly", { provider: releaseProvider(release) })}
                  </strong>
                </button>
                {!debrid.status?.connected && !torbox ? (
                  <button
                    className="sheet-choice delivery-choice"
                    data-delivery="setup-torbox"
                    disabled={pending || browser?.active}
                    onClick={() => {
                      setSheet(null);
                      configureDebrid(() => {
                        void debrid.refresh();
                        setSheet("delivery");
                      });
                    }}
                  >
                    <span className="sheet-choice-check"><Icon name="settings" /></span>
                    <span>
                      <strong>TorBox</strong>
                      <small>{t("Connect your TorBox account in App settings.")}</small>
                      <small>{t("Set up TorBox")}</small>
                    </span>
                  </button>
                ) : (
                  <button
                    className="sheet-choice delivery-choice"
                    aria-pressed={torbox}
                    data-delivery="torbox"
                    data-initial-focus={torbox || undefined}
                    disabled={pending || browser?.active || !canTorbox}
                    onClick={() => {
                      setDelivery("torbox");
                      setSheet("downloads");
                    }}
                  >
                    <span className="sheet-choice-check">
                      {torbox && <Icon name="check" />}
                    </span>
                    <strong>TorBox</strong>
                  </button>
                )}
                {(debrid.status?.connected || torbox) && (
                  <p className="sheet-note">{methodNote}</p>
                )}
              </div>
            ) : sheet === "storage" ? (
              <div className="sheet-choices">
                {drives.length ? (
                  drives.map((option) => (
                    <button
                      key={option.id}
                      className="sheet-choice"
                      aria-pressed={drive?.id === option.id}
                      data-initial-focus={drive?.id === option.id || undefined}
                      disabled={pending || browser?.active}
                      onClick={() => {
                        setDestination(option.id);
                        setSheet("downloads");
                      }}
                    >
                      <span className="sheet-choice-check">
                        {drive?.id === option.id && <Icon name="check" />}
                      </span>
                      <span>
                        <strong>{placeName(option.label)}</strong>
                        <small>
                          {t("{size} free", { size: bytes(option.freeBytes) })}
                        </small>
                        <small>
                          {t("Saves to {path}", { path: option.path })}
                        </small>
                      </span>
                    </button>
                  ))
                ) : (
                  <p className="sheet-note">
                    {t("Connect a writable drive to your PS5.")}
                  </p>
                )}
                {storageBudget}
                {drive && (
                  <meter
                    className="sheet-meter"
                    min={0}
                    max={Math.max(1, drive.totalBytes)}
                    value={Math.max(0, drive.totalBytes - drive.freeBytes)}
                    aria-label={t("Storage used")}
                  />
                )}
                <p className="sheet-note">
                  {t(
                    "Estimates include paused and failed downloads. Other apps can change free space.",
                  )}
                </p>
              </div>
            ) : (
              <>
                {initialSelection && (
                  <p className="notice">
                    {t(
                      "Your game and source are selected from the app. Check the save location, then open the download page below.",
                    )}
                  </p>
                )}
                {!communityOptions.some((option) => option.id === releaseId) && (
                  <p className="form-error" role="alert">
                    {t(
                      "The selected source is no longer available. Choose another download option to continue.",
                    )}
                  </p>
                )}
                {destination && !drive && system?.paired && (
                  <p className="form-error" role="alert">
                    {t(
                      "Your selected drive is unavailable. Connect it or choose a save location to continue.",
                    )}
                  </p>
                )}

                {showDownload && useBrowser && !isPspkg && instructions}
                {showDownload &&
                  (torbox || (debrid.status?.connected && !canTorbox)) && (
                    <p className="sheet-note">{methodNote}</p>
                  )}
                {system?.paired && (
                  <>
                    {libraryGame && (
                      <p className="collection-presence">
                        {related
                          ? null
                          : t("In your Library on {location}.", {
                              location: placeName(libraryGame.location),
                            })}
                        {collection?.kind === "related" && (
                          <button
                            onClick={() =>
                              openLibrary({
                                titleId: libraryGame.titleId,
                                path: libraryGame.path,
                                title: libraryGame.title,
                              })
                            }
                          >
                            {t("View related copy")}
                          </button>
                        )}
                      </p>
                    )}
                    {!downloadAgain &&
                      (collection?.kind === "library" ||
                        collection?.job?.status === "complete") && (
                        <button
                          className="download-another"
                          onClick={() => setDownloadAgain(true)}
                        >
                          {t("Download another copy")}
                        </button>
                      )}
                    {collection?.job?.status === "complete" && (
                      <p className="fine">
                        {t(
                          "Download history records a completed transfer. Find it in Library to check its current location.",
                        )}
                      </p>
                    )}
                    {showDownload &&
                      system?.paired &&
                      !torbox &&
                      release.browserAvailable &&
                      release.directAvailable !== false && (
                        <div className="provider-direct-option">
                          <button
                            onClick={startDirect}
                            disabled={downloadUnavailable}
                          >
                            {t("Download directly")}
                          </button>
                        </div>
                      )}
                    {showDownload && system.platform !== "desktop" && !drive && (
                      <p className="notice">
                        {t("Connect a writable drive to your PS5.")}
                      </p>
                    )}
                  </>
                )}
              </>
            )}
            {error && sheet !== "downloads" && (
              <p className="form-error" role="alert">
                {error}
              </p>
            )}
          </div>}
          {sheet === "downloads" && (
            <div className="sheet-confirm">
              {error && (
                <p className="form-error" role="alert">
                  {error}
                </p>
              )}
              {system?.paired && showDownload && drive && (
                <div className="sheet-estimate">
                  <span>
                    {planned
                      ? t("After your queue")
                      : t("After queue + this download")}
                  </span>
                  <strong>
                    {afterDownload < 0
                      ? t("{size} short", { size: bytes(-afterDownload) })
                      : bytes(afterDownload)}
                  </strong>
                </div>
              )}
              <button
                className="primary sheet-primary"
                data-long-label={actionLabel.length > 34 || undefined}
                disabled={
                  system?.paired && showDownload
                    ? downloadUnavailable || (useBrowser && !browser?.available)
                    : pending
                }
                onClick={() => primaryAction()}
                aria-describedby={
                  showDownload && useBrowser
                    ? "provider-browser-steps"
                    : undefined
                }
              >
                {actionLabel}
              </button>
              <p className="sheet-footnote">
                {t(
                  "Saves a file to your console. Installation and launching are separate.",
                )}
              </p>
            </div>
          )}
        </GameSheet>
      )}
    </section>
  );
}
