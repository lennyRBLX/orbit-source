import { useEffect, useState } from "react";
import { api } from "./api";
import { t } from "./i18n";
import { Modal, Icon } from "./components";
import fallback from "./support-default.json";
import "./support.css";

export function SupportPanel() {
  const [links, setLinks] = useState(fallback.links);
  const [selected, select] = useState(fallback.links[0].url);
  const [showQR, setShowQR] = useState(false);
  useEffect(() => {
    let alive = true;
    api<typeof fallback>("/support").then(value => {
      if (alive && Array.isArray(value.links) && value.links.length) setLinks(value.links);
    }).catch(() => {});
    return () => { alive = false; };
  }, []);
  const current = links.find(link => link.url === selected) || links[0];
  const size = current.qr.length;
  const path = current.qr.flatMap((row, y) => [...row].flatMap((v, x) => v === "1" ? [`M${x+4} ${y+4}h1v1h-1z`] : [])).join("");
  return <section className="orbit-support" aria-label={t("Support Orbit")}>
    <h2>{t("Support Orbit")}</h2>
    <p className="support-lead">{t("Orbit stays free.")}</p>
    <p>{t("If Orbit is useful to you, you can help cover the costs of AI tools, test consoles, hardware and servers.")}</p>
    <p className="support-optional">{t("Support is entirely optional.")}</p>
    {links.length > 1 && <div className="support-providers" role="group" aria-label={t("Support options")}>
      {links.map(link => <button key={link.url} type="button" aria-pressed={current.url === link.url} onClick={() => select(link.url)}>
        {link.label}
      </button>)}
    </div>}
    <div className="support-destination">
      <strong>{current.label}</strong><span className="support-url">{current.url.replace(/^https:\/\//, "")}</span>
    </div>
    <div className="support-actions">
      <a href={current.url} className="primary support-open" target="_blank" rel="noopener noreferrer">{t("Open support page")} ↗</a>
      <button type="button" aria-expanded={showQR} onClick={() => setShowQR(value => !value)}>{showQR ? t("Hide QR code") : t("Show QR code")}</button>
    </div>
    {showQR && <figure className="support-qr">
      <svg viewBox={`0 0 ${size+8} ${size+8}`} role="img" aria-label={t("Scan with your phone")} shapeRendering="crispEdges">
        <rect width={size+8} height={size+8} fill="white"/><path d={path} fill="black"/>
      </svg>
      <figcaption>{t("Scan with your phone")}</figcaption>
    </figure>}
  </section>;
}
export function SupportEntry() {
  const [open, setOpen] = useState(false);
  return <>
    <button className="support-entry" type="button" aria-haspopup="dialog" onClick={() => setOpen(true)}>
      <Icon name="heart"/><span><strong>{t("Support Orbit")}</strong><small>{t("Optional support for development")}</small></span><span aria-hidden="true">›</span>
    </button>
    {open && <div data-support-dialog><Modal title={t("Support Orbit")} close={() => setOpen(false)}><SupportPanel/></Modal></div>}
  </>;
}
