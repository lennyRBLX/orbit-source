import { useEffect, useRef, useState } from "react";
import { api } from "./api";
import { Modal } from "./components";
import { t } from "./i18n";
import type { FileOperation } from "./types";

export interface Verification {
  token: string;
  checkedFiles: number;
  fixCount: number;
  warningCount: number;
  complete: boolean;
  canRepair: boolean;
  issues: { kind: string; path: string; repairable: boolean; mode?: string; reference?: string; replacement?: string }[];
}
const labels: Record<string, string> = {
  permissions: /* i18n */ "File permissions",
  "filename-case": /* i18n */ "Filename capitalization",
  "missing-reference": /* i18n */ "Linked file not found",
  "ambiguous-reference": /* i18n */ "More than one matching file",
  "unsupported-reference": /* i18n */ "File link needs review",
  "unsupported-entry": /* i18n */ "File needs manual review",
};
export function VerificationReport({ report }: { report: Verification }) {
  return <section className="game-verification" aria-label={t("Verify & Repair")}>
    <p>{t("Files checked: {files} · Repairs proposed: {fixes} · Warnings: {warnings}", { files: report.checkedFiles, fixes: report.fixCount, warnings: report.warningCount })}</p>
    {!report.complete && <p className="form-error">{t("The check is incomplete. Review the warnings and check the folder again before repairing.")}</p>}
    {report.complete && !report.fixCount && !report.warningCount && <p>{t("No permission or filename problems found.")}</p>}
    {report.issues.length > 0 && <ol className="verification-issues" tabIndex={0} aria-label={t("Proposed repairs and warnings")}>
      {report.issues.map((issue, index) => <li key={index}>
        <strong>{t(labels[issue.kind] || labels["unsupported-entry"])}</strong>
        <code dir="ltr">{issue.path}</code>
        {issue.kind === "permissions" && <span>{t("Restore the permissions needed to read or run this item.")}</span>}
        {issue.reference && <code dir="ltr">{issue.reference}{issue.replacement ? ` → ${issue.replacement}` : ""}</code>}
        {!issue.repairable && <span>{t("No automatic change")}</span>}
      </li>)}
    </ol>}
    {report.canRepair && <p className="fine">{t("Only the changes shown here will be applied. Orbit keeps backups of edited configuration files.")}</p>}
  </section>;
}
// Downloads can open the same PS5 verification worker without navigating away.
export function GameVerificationDialog({ storageId, path, close }: { storageId: string; path: string; close: () => void }) {
  const [operation, setOperation] = useState<FileOperation | null>(null);
  const [error, setError] = useState("");
  const [statusError, setStatusError] = useState("");
  const [busy, setBusy] = useState(false);
  const id = useRef("");
  const started = useRef(false);
  const run = async (action: "verify" | "repair") => {
    setBusy(true); setError(""); setStatusError("");
    try {
      const result = await api<{ operation: FileOperation }>("/files/action", { action, storageId, path, ...(action === "repair" ? { preview: operation?.verification?.token } : {}) });
      id.current = result.operation.id; setOperation(result.operation);
    } catch (e) { setError((e as Error).message); } finally { setBusy(false); }
  };
  useEffect(() => { if (!started.current) { started.current = true; void run("verify"); } }, []);
  useEffect(() => {
    let live = true;
    const timer = setInterval(() => { if (id.current) void api<{ operation: FileOperation }>("/files/status").then(result => {
      if (live && result.operation?.id === id.current) { setOperation(result.operation); setStatusError(""); }
      else if (live) setStatusError(t("This check is no longer available. Check the game again."));
    }).catch(e => { if (live) setStatusError(e.message); }); }, 1000);
    return () => { live = false; clearInterval(timer); };
  }, []);
  const ended = operation && ["complete", "failed", "cancelled"].includes(operation.state);
  return <Modal title={t("Verify & Repair")} close={close}>
    <h2>{t("Verify & Repair")}</h2><p><code dir="ltr">{path}</code></p>
    {operation?.verification ? <VerificationReport report={operation.verification} /> : !error && <p role="status">{t("Checking game files…")}</p>}
    {(error || statusError || operation?.error) && <p role="alert">{t(error || statusError || operation!.error)}</p>}
    <div className="actions">
      {(error || statusError || (ended && (operation.state === "failed" || operation.verification?.complete === false))) && <button disabled={busy} onClick={() => void run("verify")}>{t("Try again")}</button>}
      {ended && operation.verification?.canRepair && <button className="primary" disabled={busy || !!error || !!statusError} onClick={() => void run("repair")}>{t("Apply proposed repairs")}</button>}
      <button onClick={close}>{t("Close")}</button>
    </div>
  </Modal>;
}
