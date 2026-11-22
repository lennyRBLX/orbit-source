import { useCardInsights } from "./interfaces/choice";
import type { GameInsights as Insights } from "./insights";
import { playStyleLabels, playtime } from "./insights";
import { formatNumber, t } from "./i18n";
export function insightTime(minutes: number) { return t("About {hours} h", { hours: formatNumber(minutes / 60, { maximumFractionDigits: 1 }) }); }
export function InsightSummary({ insights }: { insights?: Insights }) {
  const show = useCardInsights();
  if (!show || !insights) return null;
  const minutes = playtime(insights);
  return <span className="insight-summary">
    {insights.metacritic && <span aria-label={t("Metacritic: {score} out of 100", {score:insights.metacritic.score})}>Metacritic {formatNumber(insights.metacritic.score)}</span>}
    {minutes != null && <span>{insightTime(minutes)}</span>}
  </span>;
}
export function GameInsights({ insights }: { insights?: Insights }) {
  if (!insights) return null;
  const h = insights.howLongToBeat, m = insights.metacritic;
  return <section className="game-insights" aria-label={t("Reviews and playtime")}>
    {m && <p><a href={m.url} target="_blank" rel="noreferrer">Metacritic</a>: <strong>{formatNumber(m.score)} / 100</strong> · {m.platform.toUpperCase()}</p>}
    {h && <><strong><a href={h.url} target="_blank" rel="noreferrer">HowLongToBeat</a></strong><dl>
      {[h.mainMinutes,h.extrasMinutes,h.completionistMinutes].map((value,index)=>value != null && <div key={index}><dt>{t(playStyleLabels[index])}</dt><dd>{insightTime(value)}</dd></div>)}
    </dl></>}
    {h && <p className="fine">{t("Playtimes are community estimates and vary by player.")}</p>}
  </section>;
}
