import { useEffect, useState } from "react";
import { api } from "./api";
import { t } from "./i18n";

export type VikingPassStatus = { saved: boolean; state: "none" | "signed-in" | "signed-out" | "unknown"; busy: boolean };
const PASS_GUIDE = "https://github.com/saawant12/orbit-store-ps5/blob/main/guides/vikingfile.md#use-a-pass-from-another-device";

type Status = { id: string; vikingAccountAvailable?: boolean; vikingBrowserState?: string; accountPage?: boolean; active: boolean; state: string; message: string };

// Authentication stays on the official provider page in the same PS5 browser
// used for file capture. A successful launch is not a successful sign-in.
export function VikingAccount() {
  const [status, setStatus] = useState<Status | null>(null);
  const [sending, setSending] = useState(false);
  const [error, setError] = useState("");
  useEffect(() => {
    let live = true;
    api<Status>("/browser").then(s => { if (live) setStatus(s); })
      .catch(e => { if (live) setError(e.message); });
    return () => { live = false; };
  }, []);
  async function open(check = false) {
    if (sending) return;
    setSending(true); setError("");
    try {
      const opened = await api<Status>("/browser", { action: check ? "viking-check" : "viking-account", returnTarget: "browser" });
      // Report launch failures from the worker, without treating an open page
      // as proof of authentication. The user finishes sign-in on the PS5.
      for (let i = 0; i < 40; i++) {
        await new Promise(resolve => setTimeout(resolve, 250));
        const next = await api<Status & { id: string }>("/browser");
        if (next.id !== opened.id) break;
        setStatus(next);
        if (next.state === "failed") { setError(next.message); break; }
        if (!next.active) break;
      }
    } catch (e) { setError((e as Error).message); }
    finally { setSending(false); }
  }
  useEffect(() => {
    const refresh = () => { if (status?.vikingAccountAvailable && !status.active && !sending) void open(true); };
    window.addEventListener("focus", refresh);
    return () => window.removeEventListener("focus", refresh);
  }, [status?.active, status?.vikingAccountAvailable, sending]);
  return <section className="updates-panel archive-account-settings" aria-labelledby="viking-account-heading" tabIndex={-1}>
    <h2 id="viking-account-heading">Vikingfile</h2>
    <p>{t("Sign in on Vikingfile, then return to Orbit.")}</p>
    <p className="fine">{t("Orbit uses your Vikingfile sign-in when requesting download links. If verification is needed, its page opens.")}</p>
    <p className="fine">{t("Enter your Vikingfile account code (hash), complete verification if asked, then select Login. Return to Orbit when finished. Manage or sign out on the same account page.")}</p>
    <p>{status?.vikingBrowserState === "signed-in" ? t("Signed in") : status?.vikingBrowserState === "signed-out" ? t("Not signed in") : t("Sign-in could not be confirmed")}</p>
    <div className="dialog-actions"><button disabled={sending || !status?.vikingAccountAvailable || status.active}
      onClick={() => void open()}>{t("Open Vikingfile account")}</button>
      <button disabled={sending || !status?.vikingAccountAvailable || status.active}
        onClick={() => void open(true)}>{t("Check sign-in")}</button></div>
    {status && !status.vikingAccountAvailable && <p>{status.vikingAccountAvailable === undefined
      ? t("Update Orbit to open provider pages.") : t("Provider verification is available on the PS5 only.")}</p>}
    {error && <p className="form-error" role="alert">{t(error)}</p>}
    <VikingPass />
  </section>;
}

// The second way to sign in: a pass (Vikingfile's PHPSESSID cookie) copied
// from a browser signed in on another device, for PS5s whose browser can't
// run Vikingfile's sign-in page. Orbit checks it with Vikingfile, keeps it on
// the PS5 and never sends it back.
export function VikingPass() {
  const [status, setStatus] = useState<VikingPassStatus | null>(null);
  const [value, setValue] = useState("");
  const [sending, setSending] = useState(false);
  const [error, setError] = useState("");
  async function act(body: Record<string, string>) {
    setSending(true); setError("");
    try {
      setStatus(await api<VikingPassStatus>("/viking-pass", body));
    } catch (e) { setError((e as Error).message); }
    // Like a password, the pasted pass isn't kept in the page.
    finally { if (body.action === "save") setValue(""); setSending(false); }
  }
  useEffect(() => {
    let live = true;
    api<VikingPassStatus>("/viking-pass").then(next => {
      if (!live) return;
      setStatus(next);
      // Not checked since Orbit started: ask Vikingfile once.
      if (next.saved && next.state === "unknown" && !next.busy) void act({ action: "check" });
    }).catch(e => { if (live) setError((e as Error).message); });
    return () => { live = false; };
  }, []);
  const busy = sending || !!status?.busy;
  const state = !status?.saved ? t("No pass saved")
    : status.state === "signed-in" ? t("Pass saved and signed in")
    : status.state === "signed-out" ? t("Vikingfile has signed this pass out. Paste a new one.")
    : t("Pass saved. Sign-in not confirmed yet.");
  return <>
    <h3 className="viking-pass-heading">{t("Use a sign-in pass from another device")}</h3>
    <p className="fine">{t("If Vikingfile's sign-in page doesn't work on your PS5, sign in on vikingfile.com in a browser on your computer or phone. Copy the value of its PHPSESSID cookie and paste it here.")}</p>
    <p className="fine">{t("Orbit checks the pass with Vikingfile and keeps it only on your PS5. Treat it like a password.")}</p>
    <p role="status">{state}</p>
    <form onSubmit={e => { e.preventDefault(); if (value.trim()) void act({ action: "save", pass: value.trim() }); }}>
      <label className="field" htmlFor="viking-pass">{t("Sign-in pass (PHPSESSID)")}</label>
      <input id="viking-pass" type="password" autoComplete="off" spellCheck={false} value={value}
        disabled={busy || !status} onChange={e => setValue(e.target.value)} />
      <p className="fine"><a href={PASS_GUIDE} target="_blank" rel="noreferrer">{t("How to copy the pass")}</a></p>
      <div className="dialog-actions">
        <button type="submit" disabled={busy || !status || !value.trim()}>{sending ? t("Checking…") : t("Save pass")}</button>
        {status?.saved && <button type="button" disabled={busy} onClick={() => void act({ action: "check" })}>{t("Check pass")}</button>}
        {status?.saved && <button type="button" disabled={busy} onClick={() => void act({ action: "forget" })}>{t("Forget pass")}</button>}
      </div>
    </form>
    {error && <p className="form-error" role="alert">{error}</p>}
  </>;
}
