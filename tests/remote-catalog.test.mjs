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
    `,
    resolveDir: fileURLToPath(new URL("..", import.meta.url)),
  },
  bundle: true,
  write: false,
  format: "esm",
  platform: "node",
});
const { groupGames, releaseMatches, releaseState } = await import(
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
