import type { Release, System } from "./types";
import { t } from "./i18n";

type Facts = Pick<Release, "firmwareIssue" | "maxFirmware" | "requiredFirmware" | "mounter">;

// What a download option needs from the console's firmware, for its row in a list.
// Labels only: an option that won't run on this PS5 stays available, listed last.
export function firmwareLabel(r: Facts) {
  if (r.mounter === "kraken" && !r.firmwareIssue) return t("Runs with Kraken Mount");
  if (r.firmwareIssue === "ps5-package") return t("Needs firmware {version} or lower", { version: r.maxFirmware || "" });
  if (r.firmwareIssue === "newer-firmware") return t("Needs firmware {version} or later", { version: r.requiredFirmware || "" });
  return [
    r.requiredFirmware && t("Firmware {version} or later", { version: r.requiredFirmware }),
    r.maxFirmware && t("Firmware {version} or lower", { version: r.maxFirmware }),
  ]
    .filter(Boolean)
    .join(" · ");
}

// The sentence for the chosen option when it won't run on this PS5.
export function firmwareWarning(r: Facts, system: Pick<System, "firmware"> | null) {
  const current = system?.firmware || "";
  if (r.firmwareIssue === "ps5-package")
    return t("This download needs firmware {version} or lower. This PS5 is on {current}.", { version: r.maxFirmware || "", current });
  if (r.firmwareIssue === "newer-firmware")
    return t("This download needs firmware {version} or later. This PS5 is on {current}.", { version: r.requiredFirmware || "", current });
  return "";
}

// Options that run on this PS5 first, each group in catalogue order.
export function compatibleFirst<T extends Facts>(releases: T[]) {
  return [...releases.filter((r) => !r.firmwareIssue), ...releases.filter((r) => r.firmwareIssue)];
}

export function downloadFirmwareNote(job: Facts & { firmwareSource?: string; consoleFirmware?: string; backport?: boolean; titleId?: string }) {
  // Kraken Mount applies a backport only from <scan folder>/backports/<TITLE_ID>/ (or /data/homebrew/backports/),
  // not from inside the game's folder (kraken-mount src/scan.rs backport_for).
  if (job.backport && job.firmwareIssue === "newer-firmware")
    return t("This download needs firmware {version} or later. This PS5 is on {current}. It includes a backport for older firmware. To use it with Kraken Mount, move the folder that contains fakelib to /data/homebrew/backports/ and name it {titleId}. You can do this in {place}.", {
      version: job.requiredFirmware || "", current: job.consoleFirmware || "", titleId: job.titleId || "",
      place: `${t("Library")} → ${t("Files")}`,
    });
  const warning = firmwareWarning(job, { firmware: job.consoleFirmware });
  if (warning) return warning;
  if (job.firmwareSource === "param")
    return t("Game files declare firmware {version} or later. This does not confirm game compatibility.", { version: job.requiredFirmware || "" });
  return job.maxFirmware || job.mounter ? firmwareLabel(job) : "";
}
