import { useState } from "react";
import { api } from "./api";
import { Icon } from "./components";
import { t } from "./i18n";

// Whether ZIP, 7z and RAR downloads are extracted, and whether an archive is
// deleted once its files are extracted. `extract` is unset on services that
// always extract.
export function ArchiveSetting({
  enabled,
  extract,
  refresh,
}: {
  enabled: boolean;
  extract?: boolean;
  refresh: () => Promise<unknown>;
}) {
  const [pending, setPending] = useState(false),
    [error, setError] = useState("");
  const change = async (body: Record<string, boolean>) => {
    setPending(true);
    setError("");
    try {
      await api("/settings/extraction", body);
      await refresh();
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setPending(false);
    }
  };
  const extracting = extract !== false;
  return (
    <section className="archive-setting">
      <h2 className="download-storage-heading">{t("Compressed downloads")}</h2>
      <p>
        {extracting
          ? t(
              "ZIP, 7z and RAR downloads are extracted on the drive after they finish. Orbit tries common archive passwords automatically. If needed, you can enter a password in Downloads.",
            )
          : t(
              "ZIP, 7z and RAR downloads are kept as archives, so you can extract them with another tool. This applies to downloads you add from now on.",
            )}
      </p>
      {typeof extract === "boolean" && (
        <button
          type="button"
          role="checkbox"
          aria-checked={extracting}
          className="source-acknowledgement"
          disabled={pending}
          onClick={() => void change({ extractArchives: !extracting })}
        >
          <span className="choice-check" aria-hidden="true">
            {extracting && <Icon name="check" />}
          </span>
          <span>{t("Extract archives after downloading")}</span>
        </button>
      )}
      {extracting && (
        <button
          type="button"
          role="checkbox"
          aria-checked={enabled}
          className="source-acknowledgement"
          disabled={pending}
          onClick={() => void change({ deleteArchives: !enabled })}
        >
          <span className="choice-check" aria-hidden="true">
            {enabled && <Icon name="check" />}
          </span>
          <span>{t("Delete archives after extracting")}</span>
        </button>
      )}
      {error && (
        <p className="form-error" role="alert">
          {error}
        </p>
      )}
    </section>
  );
}
