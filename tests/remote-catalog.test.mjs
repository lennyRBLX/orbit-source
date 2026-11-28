import assert from "node:assert/strict";
import test from "node:test";
import { build } from "esbuild";
import { fileURLToPath } from "node:url";

// Load the same catalogue, Browse and collection code that Vite bundles.
const { outputFiles } = await build({
  stdin: {
    contents: `
      export { groupGames } from "./src/catalog";
      export { releaseMatches } from "./src/browse";
      export { releaseState } from "./src/collection";
      export { formatLabel, releaseSize, releaseVersion } from "./src/types";
      export { preferredRelease, sortDownloadLinks } from "./src/downloadChoice";
    `,
    resolveDir: fileURLToPath(new URL("..", import.meta.url)),
  },
  bundle: true,
  write: false,
  format: "esm",
  platform: "node",
});
const { groupGames, releaseMatches, releaseState, formatLabel, releaseSize, releaseVersion, preferredRelease, sortDownloadLinks } = await import(
  `data:text/javascript;base64,${Buffer.from(outputFiles[0].text).toString("base64")}`
);

const curated = {
  id: "curated-option", gameId: "curated-title", title: "Game", titleId: "PPSA11111",
  platform: "ps5", format: "Game folder", sourceId: "archive", sizeBytes: 2000,
};
const mirror = { ...curated, id: "rs-remote-option", sourceId: "remote" };
const homebrew = {
  id: "rs-homebrew-option", gameId: "rs-homebrew-title", title: "Homebrew app",
  titleId: "TEST10000", filename: "app.pkg", platform: "ps5", format: "PKG",
  sourceId: "remote", homebrew: true, sizeBytes: 0,
};
const filters = { platform: "", source: [], format: [], region: "", size: "" };

test("community options join the curated game and standalone titles have a Homebrew filter", () => {
  const [game, standalone] = groupGames([curated, mirror, homebrew]);
  assert.equal(game.id, curated.gameId);
  assert.deepEqual(game.releases.map((release) => release.id), [curated.id, mirror.id]);
  assert.equal(standalone.homebrew, true);
  assert.equal(standalone.platform, "ps5");
  assert.equal(releaseMatches(mirror, { ...filters, source: ["remote"], platform: "ps5" }), true);
  assert.equal(releaseMatches(curated, { ...filters, source: ["remote"] }), false);
  assert.equal(releaseMatches(homebrew, { ...filters, platform: "homebrew" }), true);
  assert.equal(releaseMatches(homebrew, { ...filters, platform: "ps5" }), false);
  assert.equal(releaseMatches(homebrew, { ...filters, size: "small" }), false);
});

test("a job remains associated with a size-unknown option after the queue learns its size", () => {
  const job = {
    releaseId: homebrew.id, titleId: homebrew.titleId, filename: homebrew.filename,
    format: homebrew.format, total: 2048, status: "complete",
  };
  assert.equal(releaseState(homebrew, [job], []).job, job);
  assert.equal(releaseState({ ...homebrew, sizeBytes: 4096 }, [job], []), undefined);
});

test("a resolved community page download still belongs to its catalogue option", () => {
  const page = { ...homebrew, filename: "", format: "Unknown", browserAvailable: true };
  const job = { releaseId: page.id, titleId: page.titleId, filename: "game.zip",
    format: "Game folder", total: 3482, status: "queued" };
  assert.equal(releaseState(page, [job], []).job, job);
  assert.equal(releaseState({ ...page, id: "different-mirror" }, [job], []), undefined);
});

test("provider options have a usable label while the file type is pending", () => {
  assert.equal(formatLabel({ format: "Unknown" }), "Download");
  assert.equal(formatLabel({ format: "Unknown", compression: "zip" }), "ZIP");
  assert.equal(formatLabel({ format: "PKG", compression: "rar" }), "RAR · PKG");
});

test("version labels prefer source metadata and use one prefix without dropping digits", () => {
  assert.equal(releaseVersion({ version: "02.120" }), "v02.120");
  assert.equal(releaseVersion({ version: "v02.120" }), "v02.120");
  assert.equal(releaseVersion({ version: "VV02.120" }), "v02.120");
  assert.equal(releaseVersion({ resolvedVersion: "02.120" }), "v02.120");
  assert.equal(releaseVersion({ version: "01.00", resolvedVersion: "02.120" }), "v01.00");
  assert.equal(releaseVersion({}), "");
});

test("TorBox coverage wins the default and its size follows the selected delivery", () => {
  const torbox = { ...mirror, torboxHost: "buzzheavier.com", sizeBytes: 0, torboxSizeBytes: 118400000000 };
  const hosts = new Set(["buzzheavier.com"]);
  const last = { source: curated.sourceId, format: curated.format };
  assert.equal(preferredRelease([curated, torbox], undefined, last, hosts), torbox);
  assert.equal(preferredRelease([curated, torbox], { ...filters, source: ["archive"] }, last, hosts), curated);
  assert.equal(releaseSize(torbox, true), "118.4 GB");
  assert.equal(releaseSize(torbox), "Unknown size");
  const cached = { ...torbox, id: "cached", torboxCached: true };
  assert.equal(preferredRelease([torbox, cached], undefined, last, hosts), cached);
  const undersized = { ...cached, torboxSizeBytes: 9, sizeEstimateBytes: 109_700_000_000 };
  assert.equal(preferredRelease([undersized, torbox], undefined, last, hosts), torbox);
  assert.equal(releaseSize(undersized, true), "≈109.7 GB");
  assert.equal(releaseSize({ ...cached, torboxSizeBytes: 500_000_000 }, true), "500.0 MB");
});

test("catalogue download links sort by numeric version, TorBox status and format", () => {
  const option = (id, fields = {}) => ({ ...curated, id, version: "2.10", ...fields });
  const links = [
    option("folder", { torboxCached: true }),
    option("older", { version: "2.9", format: "PKG", torboxCached: true }),
    option("available", { format: "PKG" }),
    option("unavailable", { format: "PKG", torboxCached: true }),
    option("lz4", { contentFormat: "LZ4", torboxCached: true }),
    option("ffpkg", { format: "FFPKG", torboxCached: true }),
    option("zstd", { assetCompression: "ZSTD", torboxCached: true }),
    option("pkg", { version: "v02.10.0", format: "PKG", torboxCached: true }),
    option("newer", { version: null, resolvedVersion: "v2.11" }),
    option("unknown", { version: null, format: "PKG", torboxCached: true }),
  ];
  const sorted = sortDownloadLinks(links, (link) => link.id !== "unavailable");
  assert.deepEqual(sorted.map(({ id }) => id), [
    "newer", "ffpkg", "pkg", "zstd", "lz4", "folder", "available", "unavailable", "older", "unknown",
  ]);
  assert.equal(links[0].id, "folder");
});

test("PSPKG download links use their own TorBox status and preserve link IDs", () => {
  const links = [
    { id: "1", version: "2.0", format: "PKG", torboxAvailable: false },
    { id: "2", version: "2.0", format: "ZSTD", torboxAvailable: true },
    { id: "3", version: "1.0", format: "PKG", torboxAvailable: true, torboxCached: true },
    { id: "4", version: "2.0", format: "Game folder", torboxAvailable: true, torboxCached: true },
    { id: "5", version: "2.0", format: "FFPKG", torboxAvailable: true },
  ];
  assert.deepEqual(sortDownloadLinks(links, (link) => link.torboxAvailable).map(({ id }) => id),
    ["4", "5", "2", "1", "3"]);
});
