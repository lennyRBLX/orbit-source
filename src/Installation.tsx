import { GameVerificationDialog } from "./GameVerification";
import { AddonInfo } from "./AddonInfo";
import { useState } from "react";
import { Modal } from "./components";
import { Select } from "./Select";
import { bytes, type Job } from "./types";
import { t } from "./i18n";
import { downloadFirmwareNote } from "./firmware";

export interface PackageInstallation {
  id: string;
  filename: string;
  platform: string;
  state: string;
  message: string;
  received: number;
  total: number;
}
export interface Installation {
  id: string;
  available: boolean;
  state: string;
  message: string;
  packages: PackageInstallation[];
  shadowMount: string;
  shadowMountMessage: string;
  verifications?: { storageId: string; path: string; fixCount: number; warningCount: number; complete: boolean }[];
  /** Get everything: waiting for the download before it, or stopped because that one wasn't installed. */
  chain?: "" | "waiting" | "blocked";
  chainTitle?: string;
}
/** Get everything's install order, in this language (the service's message is English). */
export function chainMessage(i: Installation) {
  if (i.chain === "waiting") return t("Installs after {title}.", { title: i.chainTitle || "" });
  if (i.chain === "blocked") return t("Install {title} first, then select Install.", { title: i.chainTitle || "" });
  return "";
}
export function installationLabel(job: Job) {
  const i = job.installation;
  if (i?.packages.some(p => ["ready", "error"].includes(p.state))) return t("Install on PS5");
  if (i?.packages.length) return t("Installation details");
  return i?.shadowMount && i.shadowMount !== "none" ? t("ShadowMount status") : t("Check downloaded files");
}
export function InstallationSummary({ job }: { job: Job }) {
  const firmware = downloadFirmwareNote(job);
  const note = firmware && <p className="installation-summary">{firmware}</p>;
  if (job.status !== "complete" || !job.installation) return note || null;
  const i = job.installation;
  const active = i.packages.find(p => ["queued", "installing", "unconfirmed", "error", "blocked"].includes(p.state));
  const installed = i.packages.length > 0 && i.packages.every(p => p.state === "installed");
  const message = chainMessage(i) || active?.message || (installed ? t("Installed on PS5.") : i.shadowMountMessage || i.message);
  return <>{note}{message && <p className="installation-summary" role="status">{t(message)}</p>}</>;
}
export function InstallationDialog({ job, close, run, busy, error, deletePackages = false }: {
  job: Job; close: () => void; run: (job: Job, action: string) => Promise<void>; busy: boolean; error: string; deletePackages?: boolean;
}) {
  const [selected, setSelected] = useState("");
  const [checking, setChecking] = useState<{storageId:string;path:string}|null>(null);
  const i = job.installation;
  const packages = i?.packages || [];
  const pkg = packages.find(p => p.id === selected) || packages[0];
  return <Modal title={t("Install on PS5")} close={close}>
    <h2>{t("Downloaded files")}</h2>
    <p><bdi>{job.title}</bdi></p>
    <AddonInfo item={job} />
    {downloadFirmwareNote(job) && <p className="fine">{downloadFirmwareNote(job)}</p>}
    {packages.length > 1 && <Select label={t("Package")} value={pkg?.id || ""} disabled={busy}
      onChange={setSelected} options={packages.map(p => ({value:p.id,label:p.filename}))} />}
    {pkg && <>
      <p className="download-location"><code dir="ltr">{pkg.filename}</code></p>
      <p>{deletePackages
        ? t("The PS5 installs the selected package to its configured installation storage. Once it’s installed, Orbit deletes the downloaded package.")
        : t("The PS5 installs the selected package to its configured installation storage. Your downloaded file stays on its drive.")}</p>
      {!i?.available && pkg.state === "ready" && <p>{t("Enable the local API on a ShadowMountPlus version that supports package installation.")}</p>}
      <p role="status">{pkg.state === "ready" && i && chainMessage(i) ? chainMessage(i) : t(pkg.message)}</p>
      {pkg.state === "installing" && pkg.total > 0 && <><progress max={pkg.total} value={Math.min(pkg.received, pkg.total)} />
        <p>{bytes(pkg.received)} / {bytes(pkg.total)}</p></>}
      {pkg.platform === "ps5" && <p className="fine">{t("PS5 game packages require compatible firmware and jailbreak support. Installation does not change game compatibility.")}</p>}
    </>}
    {!pkg && (!i?.shadowMount || i.shadowMount === "none") && <p role="status">{t(i?.message || (i?.state === "ready" ? "No installable packages were found in this download." : "Checking downloaded files…"))}</p>}
    {i?.verifications?.map(check => <div key={check.path}><p>{t("Repairs proposed: {fixes} · Warnings: {warnings}", {fixes:check.fixCount,warnings:check.warningCount})}</p><button onClick={()=>setChecking(check)}>{t("Verify & Repair")}</button></div>)}
    {checking && <GameVerificationDialog {...checking} close={()=>setChecking(null)} />}
    {i?.shadowMountMessage && <p role="status">{t(i.shadowMountMessage)}</p>}
    {error && <p role="alert">{t(error)}</p>}
    <div className="actions">
      {pkg && ["ready", "error"].includes(pkg.state) && <button className="primary" disabled={busy || !i?.available}
        onClick={() => void run(job, `install-${pkg.id}`)}>{t("Install on PS5")}</button>}
      {pkg?.state === "unconfirmed" && <button disabled={busy} onClick={() => void run(job, `check-${pkg.id}`)}>{t("Check installation")}</button>}
      {i?.shadowMount && i.shadowMount !== "none" && !["pending", "requested", "registered"].includes(i.shadowMount) &&
        <button disabled={busy} onClick={() => void run(job, "scan-library")}>{t("Scan with ShadowMount")}</button>}
      {(!i || i.state === "error") && <button disabled={busy} onClick={() => void run(job, "check-packages")}>{t("Check downloaded files")}</button>}
      <button disabled={busy} onClick={close}>{t("Close")}</button>
    </div>
  </Modal>;
}
