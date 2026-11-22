import { useCallback, useEffect, useRef, useState } from "react";
import { api, ApiError, setToken } from "./api";
import { groupGames } from "./catalog";
import { t } from "./i18n";
import { useSnapshot } from "./useSnapshot";
import type { SavedFilter } from "./browse";
import type {
  Bundle,
  BrowserSession,
  Game,
  Release,
  Job,
  SourceId,
  SourceSettings,
  Storage,
  System,
} from "./types";
export function useOrbit() {
  const [addons, setAddons] = useState<Game[]>([]);
  // Get everything's Download all: polled while one runs or while a page shows its progress.
  const [bundle, setBundle] = useSnapshot<Bundle | null>(null);
  const bundleWatchers = useRef(0),
    bundleLoaded = useRef(false),
    bundleActive = useRef(false);
  const [games, setGames] = useState<Game[]>([]),
    [favorites, setFavorites] = useSnapshot<string[]>([]),
    [savedFilters, setSavedFilters] = useSnapshot<SavedFilter[]>([]),
    [jobs, setJobs] = useSnapshot<Job[]>([]),
    [storage, setStorage] = useSnapshot<Storage[]>([]);
  const [ready, setReady] = useState(false);
  const [browser, setBrowser] = useSnapshot<BrowserSession | null>(null);
  const [system, setSystem] = useSnapshot<System | null>(null),
    [sources, setSources] = useSnapshot<SourceSettings | null>(null),
    [error, setError] = useState("");
  const alive = useRef(true),
    polling = useRef<Promise<void> | null>(null),
    catalogKey = useRef(""),
    localSessionLoaded = useRef(false),
    failures = useRef(0),
    loadedVersion = useRef(""),
    latestVersion = useRef("");
  const refresh = useCallback(() => {
    if (polling.current) return polling.current;
    const request = (async () => {
      try {
        const [sys, sourceSettings] = await Promise.all([
          api<System>("/system"),
          api<SourceSettings>("/sources"),
        ]);
        if (!alive.current) return;
        const key = JSON.stringify([
          sys.version,
          sys.instanceId,
          sys.catalogueRevision,
          sourceSettings.noticeVersion,
          sourceSettings.acknowledged,
          sourceSettings.enabled,
          sourceSettings.remoteGeneration,
          sourceSettings.options.map((source) => [
            source.id,
            source.releaseCount,
          ]),
        ]);
        if (catalogKey.current !== key) {
          const [catalogue, extra] = await Promise.all([
            api<Release[]>("/catalog", undefined, { timeoutMs: 60000 }),
            api<Release[]>("/addons", undefined, { timeoutMs: 60000 }).catch(e => { if (e instanceof ApiError && e.status === 404) return []; throw e; }),
          ]);
          if (!alive.current) return;
          setGames(groupGames(catalogue));
          setAddons(groupGames(extra));
          catalogKey.current = key;
        }
        setSources(sourceSettings);
        if (
          sys.localSessionAvailable &&
          (!localSessionLoaded.current || !sys.paired)
        ) {
          const session = await api<{ token: string; pairCode: string }>(
            "/session",
          );
          if (!alive.current) return;
          setToken(session.token);
          localSessionLoaded.current = true;
          sys.paired = true;
        }
        if (sys.paired) {
          const [queue, drives, savedFavorites, filters, providerBrowser] =
            await Promise.all([
              api<Job[]>("/downloads"),
              api<Storage[]>("/storage"),
              api<string[]>("/favorites"),
              api<SavedFilter[]>("/filters"),
              api<BrowserSession>("/browser"),
            ]);
          if (alive.current) {
            setJobs(queue);
            setStorage(drives);
            setFavorites(savedFavorites);
            setSavedFilters(filters);
            setBrowser(providerBrowser);
          }
          if (alive.current && sys.bundleProtocol === 1 &&
              (!bundleLoaded.current || bundleActive.current || bundleWatchers.current > 0)) {
            const status = await api<Bundle>("/bundles");
            bundleLoaded.current = true;
            bundleActive.current = status.state === "running" || status.state === "page";
            if (alive.current) setBundle(status);
          }
        } else {
          setJobs([]);
          setStorage([]);
          setFavorites([]);
          setSavedFilters([]);
          setBrowser(null);
          setBundle(null);
          bundleLoaded.current = false;
          bundleActive.current = false;
        }
        if (!alive.current) return;
        loadedVersion.current ||= sys.version;
        latestVersion.current = sys.version;
        setSystem(sys);
        setReady(true);
        failures.current = 0;
        setError("");
      } catch (e) {
        if (!alive.current) return;
        // Orbit's own refusals show at once. A dropped or slow request on Wi-Fi
        // is retried by the next poll; only several in a row mean Orbit is gone.
        if (e instanceof ApiError) setError(e.message);
        else if (++failures.current >= 3) setError(t("Connection lost. Reconnecting to Orbit…"));
      } finally {
        polling.current = null;
      }
    })();
    polling.current = request;
    return request;
  }, [setBrowser, setBundle, setFavorites, setJobs, setSavedFilters, setSources, setStorage, setSystem]);
  useEffect(() => {
    alive.current = true;
    void refresh();
    const timer = setInterval(() => {
      void refresh();
    }, 2000);
    return () => {
      alive.current = false;
      clearInterval(timer);
    };
  }, [refresh]);
  // Reconnect asks Orbit again instead of reloading the page, which re-downloads
  // everything over a weak connection. Only a newer Orbit needs its new page.
  const reconnect = async () => {
    await polling.current;
    await refresh();
    if (alive.current && loadedVersion.current && latestVersion.current !== loadedVersion.current)
      location.reload();
  };
  const pair = async (code: string) => {
    const result = await api<{ token: string }>("/pair", { code });
    setToken(result.token);
    await polling.current;
    await refresh();
  };
  const download = async (
    releaseId: string,
    storageId: string,
    delivery: "direct" | "torbox" = "direct",
  ) => {
    await api("/downloads", { releaseId, storageId, delivery });
    await polling.current;
    await refresh();
  };
  const downloadUrl = async (url: string, storageId: string) => {
    const status = await api<BrowserSession>("/downloads/direct", { url, storageId });
    if (alive.current) setBrowser(status);
    return status;
  };
  const openProvider = async (releaseId: string, storageId: string) => {
    const status = await api<BrowserSession>("/browser", {
      action: "start",
      returnToOrbit: true,
      releaseId,
      storageId,
    });
    if (alive.current) setBrowser(status);
    await polling.current;
    await refresh();
  };
  const cancelProvider = async (id: string) => {
    const status = await api<BrowserSession>("/browser", {
      action: "cancel",
      id,
    });
    if (alive.current) setBrowser(status);
    await polling.current;
    await refresh();
  };
  const action = async (id: string, name: string, deletePartial = false, password?: string) => {
    await api(`/downloads/${id}/${name}`, { deletePartial, ...(password === undefined ? {} : { password }) });
    await polling.current;
    await refresh();
  };
  // Download all: the answer is the bundle's status.
  const changeBundle = async (body: Record<string, unknown>) => {
    const status = await api<Bundle>("/bundles", body);
    bundleActive.current = status.state === "running" || status.state === "page";
    if (alive.current) setBundle(status);
    await polling.current;
    await refresh();
    return status;
  };
  const startBundle = (body: Record<string, unknown>) => changeBundle(body);
  const cancelBundle = (id: string) => changeBundle({ action: "cancel", id });
  // A page that shows a bundle's progress keeps it fresh while open.
  const watchBundle = useCallback(() => {
    bundleWatchers.current += 1;
    return () => {
      bundleWatchers.current -= 1;
    };
  }, []);
  const clearHistory = async (status: "complete" | "cancelled") => {
    await api("/downloads/clear-history", { status });
    await polling.current;
    await refresh();
  };
  // The last favourite added or removed, for the notice every look shows.
  const [favoriteNotice, setFavoriteNotice] = useState<{ added: boolean; at: number } | null>(null);
  const favorite = async (gameId: string, value: boolean) => {
    await api<string[]>("/favorites", { gameId, favorite: value });
    setFavoriteNotice({ added: value, at: Date.now() });
    await polling.current;
    await refresh();
  };
  // Save or delete a saved filter; the answer is the whole list.
  const changeSavedFilters = async (body: Record<string, unknown>) => {
    setSavedFilters(await api<SavedFilter[]>("/filters", body));
  };
  const saveSources = async (enabled: SourceId[], noticeVersion: number) => {
    await api("/sources", { enabled, acknowledged: true, noticeVersion });
    await polling.current;
    catalogKey.current = "";
    await refresh();
  };
  return {
    games, addons,
    bundle,
    startBundle,
    cancelBundle,
    watchBundle,
    browser,
    ready,
    openProvider,
    cancelProvider,
    favorites,
    favorite,
    favoriteNotice,
    savedFilters,
    changeSavedFilters,
    clearHistory,
    jobs,
    storage,
    system,
    sources,
    error,
    refresh,
    reconnect,
    pair,
    download,
    downloadUrl,
    action,
    saveSources,
  };
}
