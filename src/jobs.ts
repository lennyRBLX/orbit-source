// What a download is doing and what can be done with it, shared by every
// layout of Downloads (Store's list and the other interfaces' views).
import type { Job, Storage, System } from "./types";
import { t } from "./i18n";

export const statusLabels: Record<Job["status"], string> = {
  queued: /* i18n */ "Queued",
  downloading: /* i18n */ "Downloading",
  retrying: /* i18n */ "Retrying",
  verifying: /* i18n */ "Verifying",
  "waiting-extraction": /* i18n */ "Waiting to unpack",
  inspecting: /* i18n */ "Inspecting archive",
  extracting: /* i18n */ "Extracting",
  paused: /* i18n */ "Paused",
  error: /* i18n */ "Needs attention",
  complete: /* i18n */ "Complete",
  cancelled: /* i18n */ "Cancelled",
};
export type DownloadView = "Active" | "Finished" | "Failed" | "Cancelled";
export const downloadViews: DownloadView[] = [
  /* i18n */ "Active",
  /* i18n */ "Finished",
  /* i18n */ "Failed",
  /* i18n */ "Cancelled",
];
// Time left at this speed: minutes, then hours and minutes past the first hour.
export function timeLeft(speed: string, seconds: number) {
  const minutes = Math.ceil(seconds / 60);
  return minutes < 60
    ? t("{speed}/s · {minutes} min remaining", { speed, minutes })
    : t("{speed}/s · {hours} h {minutes} min remaining", { speed, hours: Math.floor(minutes / 60), minutes: minutes % 60 });
}
// A retry's wait: seconds, then minutes once it's two minutes or more.
export function retryWait(seconds: number) {
  return seconds < 120 ? t("Retrying in {seconds} s", { seconds }) : t("Retrying in {minutes} min", { minutes: Math.ceil(seconds / 60) });
}
export const inactive = (job: Job) => ["complete", "cancelled"].includes(job.status);
export const waiting = (job: Job) => ["queued", "paused", "retrying"].includes(job.status);
export const running = (job: Job) =>
  ["queued", "downloading", "retrying", "verifying", "waiting-extraction", "inspecting", "extracting"].includes(job.status);

// While extracting, progress is the extraction's.
export function progressOf(job: Job) {
  const extracting = job.status === "extracting";
  const done = extracting ? job.extracted || 0 : job.received;
  const total = extracting ? job.extractTotal || 0 : job.total;
  return { extracting, done, total, percent: total > 0 ? Math.min(100, Math.max(0, (100 * done) / total)) : 0 };
}

export type JobCommand =
  | "pause"
  | "resume"
  | "retry"
  | "cancel-dialog"
  | "password"
  | "refresh-link"
  | "replace-link"
  | "free-space"
  | "move-up"
  | "move-down"
  | "remove"
  | "packages"
  | "power";
export interface JobAction {
  command: JobCommand;
  label: string; // untranslated, for t()
  disabled: boolean;
  primary?: boolean;
}

// Paused without room to unpack: Free up space on its drive, then it resumes.
export function unpackShort(job: Job) {
  return job.status === "paused" && job.error.startsWith("Not enough free space to unpack");
}

// The same buttons, in the same order and with the same conditions, as a row
// in Store's Downloads list.
export function jobActions(job: Job, { busy, drives, queued, power }: { busy: boolean; drives: Storage[]; queued: Job[]; power?: System["power"] }): JobAction[] {
  const actions: JobAction[] = [];
  busy = busy || !!job.removingFiles;
  if (job.status === "complete") actions.push({ command: "packages", label: job.installation?.packages.some(p => ["ready", "error"].includes(p.state)) ? /* i18n */ "Install on PS5" : /* i18n */ "Downloaded files", disabled: busy, primary: true });
  const active = running(job);
  const drive = drives.find((d) => d.id === job.storageId);
  if (job.canEnterArchivePassword)
    actions.push({ command: "password", label: /* i18n */ "Enter archive password", disabled: busy || !drive });
  if (job.status !== "complete") {
    actions.push({
      command: active ? "pause" : job.status === "error" ? "retry" : "resume",
      label: active
        ? /* i18n */ "Pause"
        : job.status === "error"
          ? /* i18n */ "Retry download"
          : job.status === "cancelled" && !job.received
            ? /* i18n */ "Start again"
            : /* i18n */ "Resume",
      disabled: busy || (!active && !drive),
      primary: true,
    });
    actions.push({
      command: "cancel-dialog",
      label: job.status === "cancelled" ? /* i18n */ "Delete download" : /* i18n */ "Cancel",
      disabled: busy,
    });
  }
  if (unpackShort(job) && drive) actions.push({ command: "free-space", label: /* i18n */ "Free up space", disabled: busy });
  if (job.canRefreshLink) actions.push({ command: "refresh-link", label: /* i18n */ "Refresh download link", disabled: busy });
  if (job.canReplaceLink) actions.push({ command: "replace-link", label: /* i18n */ "Replace link", disabled: busy });
  if (waiting(job)) {
    const position = queued.findIndex((q) => q.id === job.id);
    actions.push({ command: "move-up", label: /* i18n */ "↑ Move up", disabled: busy || position <= 0 });
    actions.push({ command: "move-down", label: /* i18n */ "↓ Move down", disabled: busy || position === queued.length - 1 });
  }
  if (inactive(job))
    actions.push({ command: "remove", label: /* i18n */ "Remove from history", disabled: busy });
  const shutdown = powerJobAction(job, power, busy);
  if (shutdown) actions.push(shutdown);
  return actions;
}

// Only receiver-managed downloads can be selected as a shutdown target.
export function powerJobAction(job: Job, power: System["power"], busy = false): JobAction | undefined {
  if (!power?.available || job.id.startsWith("zero-") || job.status === "cancelled") return;
  return { command: "power", label: power.enabled && power.jobId === job.id
    ? /* i18n */ "Cancel automatic shutdown" : /* i18n */ "Shut down when finished", disabled: busy || !!job.removingFiles };
}
