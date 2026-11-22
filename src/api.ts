import { t } from "./i18n";
import type { RemoteSources } from "./types";

const tokenKey = "orbit.pairing.v1";
let token = (() => {
  try {
    return localStorage.getItem(tokenKey) || "";
  } catch {
    return "";
  }
})();
export function setToken(value: string) {
  token = value;
  try {
    localStorage.setItem(tokenKey, value);
  } catch {
    /* Session-only pairing remains usable. */
  }
}
// Artwork uses the same pairing authority as every other API call. Keep tokens
// in headers, never in image URLs or persistent artwork files.
export function fetchArtwork(query: URLSearchParams, signal: AbortSignal) {
  return fetch(`/api/v1/artwork?${query}`, {
    headers: token ? { Authorization: `Bearer ${token}` } : {}, signal,
  });
}
export class ApiError extends Error {
  constructor(
    message: string,
    public status: number,
    public retryAfter?: number,
  ) {
    super(message);
  }
}
// A slow Wi-Fi phone needs longer than 12 s for the whole game list.
export async function api<T>(path: string, body?: unknown, { timeoutMs = 12000, method = body === undefined ? "GET" : "POST" }: {
  timeoutMs?: number;
  method?: "GET" | "POST" | "PATCH" | "DELETE";
} = {}): Promise<T> {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const response = await fetch(`/api/v1${path}`, {
      method,
      headers: {
        ...(token ? { Authorization: `Bearer ${token}` } : {}),
        ...(body === undefined ? {} : { "Content-Type": "application/json" }),
      },
      body: body === undefined ? undefined : JSON.stringify(body),
      signal: controller.signal,
    });
    const data = await response.json();
    if (!response.ok)
      // Orbit's fixed messages are catalogue keys, so they arrive translated.
      throw new ApiError(
        t(data.error || "Orbit could not complete this request."),
        response.status,
        typeof data.retryAfter === "number" ? data.retryAfter : undefined,
      );
    return data as T;
  } finally {
    clearTimeout(timeout);
  }
}


export const remoteSources = () => api<RemoteSources>("/remote-sources");
export const addRemoteSource = (url: string) => api<RemoteSources>("/remote-sources", { url });
export const enableRemoteSource = (id: string, enabled: boolean) =>
  api<RemoteSources>(`/remote-sources/${encodeURIComponent(id)}`, { enabled }, { method: "PATCH" });
export const removeRemoteSource = (id: string) =>
  api<RemoteSources>(`/remote-sources/${encodeURIComponent(id)}`, undefined, { method: "DELETE" });
export const refreshRemoteSources = () => api<RemoteSources>("/remote-sources/refresh", {});
