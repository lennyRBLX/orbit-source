import { useState } from "react";
import { Icon } from "./components";
import { bytes, type LibraryDrive, type LibraryGame, type Storage } from "./types";
import type { LibraryModel } from "./useLibrary";
import { placeName, t, tn } from "./i18n";

export function LibraryFeedback({
  model,
  titleId,
}: {
  model: LibraryModel;
  titleId?: string;
}) {
  const action = model.snapshot?.action;
  if (!action || (titleId && action.titleId !== titleId)) return null;
  return (
    <p
      className={
        action.state === "error"
          ? "form-error library-feedback"
          : "notice library-feedback"
      }
      role={action.state === "error" ? "alert" : "status"}
    >
      {t(action.message)}
    </p>
  );
}

// One game's details: Orbit changes nothing here. Kraken Mount manages the games it
// runs; Files copies and moves game folders.
export function GameActions({
  game,
  model,
  tiles = false,
}: {
  game: LibraryGame;
  model: LibraryModel;
  // Panorama: laid out with its tiles.
  tiles?: boolean;
}) {
  return (
    <div className={`library-game-actions${tiles ? " pn-game-actions" : ""}`}>
      <LibraryFeedback model={model} titleId={game.titleId} />
      <p className="fine">
        {t("This item supports viewing details only. Kraken Mount manages its games itself.")}
      </p>
    </div>
  );
}

// The Library games whose source is on a drive, largest first (sizes on request).
export function gamesOnDrive(model: LibraryModel, drive: LibraryDrive | undefined) {
  return (model.snapshot?.games || [])
    .filter(
      (g) =>
        g.onDrive &&
        drive &&
        (g.path === drive.path || g.path.startsWith(`${drive.path}/`)),
    )
    .sort(
      (a, b) =>
        (b.sizeBytes ?? -1) - (a.sizeBytes ?? -1) ||
        a.title.localeCompare(b.title),
    );
}

// A drive's top: Orbit reports each drive by its homebrew folder.
export const driveRoot = (drive: Storage) => drive.path.replace(/\/homebrew$/, "");
// A drive's games with their measured sizes, as Storage listed them.
export function DriveGames({
  label,
  games,
  openGame,
}: {
  label: string;
  games: LibraryGame[];
  openGame: (g: LibraryGame) => void;
}) {
  const sizesKnown = games.filter((g) => g.sizeBytes !== null).length;
  return (
    <>
      <div className="storage-overview-heading">
        <div>
          <h3>{t("Games on {drive}", { drive: placeName(label) })}</h3>
          <p>
            {tn(games.length, "{count} game", "{count} games")} ·{" "}
            {tn(sizesKnown, "{count} size measured", "{count} sizes measured")}
          </p>
        </div>
      </div>
      <p className="fine">
        {t("Orbit measures game sizes on its own. Shared source files can appear under multiple titles; drive usage includes other files.")}
      </p>
      <div className="storage-game-list">
        {games.map((g) => (
          <button className="storage-game" key={g.titleId} onClick={() => openGame(g)}>
            <span>
              <strong><bdi>{g.title}</bdi></strong>
              <small>
                {g.format} · {g.mounted ? t("Mounted") : g.installed ? t("Installed") : t("On drive")}
              </small>
            </span>
            <span>
              {g.sizeBytes !== null
                ? bytes(g.sizeBytes)
                : g.sizeStatus === "unavailable"
                  ? t("Size unavailable")
                  : t("Not measured")}
            </span>
            <span aria-hidden="true">›</span>
          </button>
        ))}
      </div>
      {!games.length && <p className="notice">{t("No Library games currently match this drive.")}</p>}
    </>
  );
}

export function StorageOverview({
  model,
  openGame,
  feedback = false,
}: {
  model: LibraryModel;
  openGame: (g: LibraryGame) => void;
  feedback?: boolean;
}) {
  const [selected, setSelected] = useState("");
  const drives = model.storage?.drives || [];
  const drive = drives.find((d) => d.id === selected) || drives[0];
  const games = gamesOnDrive(model, drive);
  return (
    <section className="storage-overview" aria-label={t("Game storage")}>
      <div className="storage-overview-heading">
        <div>
          <h2>{t("Your drives")}</h2>
          <p>{t("See available space and manage where your games live.")}</p>
        </div>
      </div>
      {model.storageError && (
        <p className="notice" role="status">
          {t(model.storageError)}
        </p>
      )}
      {!drives.length ? (
        <div className="empty compact">
          <Icon name="usb" />
          <h3>{t("No drives reported")}</h3>
          <p>{t("Connect a USB or M.2 drive to see it here.")}</p>
        </div>
      ) : (
        <>
          <div
            className="drive-grid"
            role="group"
            aria-label={t("Select drive")}
          >
            {drives.map((d) => (
              <button
                key={d.id}
                className="drive-card"
                aria-pressed={drive?.id === d.id}
                onClick={() => setSelected(d.id)}
              >
                <span className="drive-card-title">
                  <Icon name="usb" />
                  <strong>{placeName(d.label)}</strong>
                </span>
                <span>{d.path}</span>
                <span className="storage-meter">
                  <span
                    style={{
                      width: `${d.totalBytes ? Math.min(100, (100 * d.usedBytes) / d.totalBytes) : 0}%`,
                    }}
                  />
                </span>
                <strong>
                  {t("{size} free", { size: bytes(d.freeBytes) })}
                </strong>
                <span>
                  {t("{used} used of {total}", {
                    used: bytes(d.usedBytes),
                    total: bytes(d.totalBytes),
                  })}
                </span>
              </button>
            ))}
          </div>
          <DriveGames label={drive?.label || ""} games={games} openGame={openGame} />
        </>
      )}
      {feedback && <LibraryFeedback model={model} />}
    </section>
  );
}
