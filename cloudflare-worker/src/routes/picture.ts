import { json, proxyJson } from "../core/http";
import type { Env } from "../types";

type CommonsSearchResponse = {
  query?: {
    pages?: CommonsPages;
  };
};

type CommonsPage = {
  title?: string;
  imageinfo?: Array<{ url?: string }>;
};

type CommonsPages = Record<string, CommonsPage>;

type CommonsCandidate = {
  title: string;
  sourceUrl: string;
};

type CommonsListSearchResponse = {
  query?: {
    search?: Array<{ title?: string }>;
  };
};

type WikipediaThumbResponse = {
  query?: {
    pages?: Record<
      string,
      {
        title?: string;
        thumbnail?: { source?: string };
      }
    >;
  };
};

type WikipediaSummaryResponse = {
  title?: string;
  thumbnail?: { source?: string };
  originalimage?: { source?: string };
};

type OpenverseResponse = {
  results?: Array<{
    title?: string;
    thumbnail?: string;
    url?: string;
  }>;
};

export async function handlePicture(request: Request, env: Env): Promise<Response> {
  let payload: { query?: string };
  try {
    payload = (await request.json()) as { query?: string };
  } catch {
    return json({ error: "Invalid JSON body" }, 400);
  }
  const query = (payload.query || "").trim();
  if (query.length < 2) return json({ error: "Missing field: query" }, 400);

  const found = await findCommonsImage(query);
  const candidate = found.candidate;
  if (!candidate) {
    return json({ error: "No image found", detail: found.trace.slice(-12) }, 404);
  }

  const imageResp = await fetchOptimizedImage(candidate.sourceUrl);
  if (!imageResp.ok) {
    const txt = await imageResp.text();
    return proxyJson(imageResp.status, txt);
  }

  const buf = await imageResp.arrayBuffer();
  if (buf.byteLength < 128) {
    return json({ error: "Image too small" }, 502);
  }

  const headers = new Headers();
  headers.set("Content-Type", "image/jpeg");
  headers.set("Cache-Control", "public, max-age=86400");
  headers.set("x-image-title", encodeURIComponent(candidate.title.slice(0, 180)));
  headers.set("x-image-source", encodeURIComponent(candidate.sourceUrl.slice(0, 900)));
  headers.set("x-picture-trace", encodeURIComponent(found.trace.slice(-6).join(" | ").slice(0, 1000)));
  return new Response(buf, { status: 200, headers });
}

async function findCommonsImage(
  query: string
): Promise<{ candidate: CommonsCandidate | null; trace: string[] }> {
  const trace: string[] = [];
  const attempts = [query, `file:${query}`];
  for (const q of attempts) {
    const viaGenerator = await findByGeneratorSearch(q, trace);
    if (viaGenerator) return { candidate: viaGenerator, trace };
  }
  const viaList = await findByListSearch(query, trace);
  if (viaList) return { candidate: viaList, trace };
  const viaWikiThumb = await findByWikipediaThumbnail(query, trace);
  if (viaWikiThumb) return { candidate: viaWikiThumb, trace };
  const viaWikiSummary = await findByWikipediaSummary(query, trace);
  if (viaWikiSummary) return { candidate: viaWikiSummary, trace };
  const viaOpenverse = await findByOpenverse(query, trace);
  if (viaOpenverse) return { candidate: viaOpenverse, trace };
  return { candidate: null, trace };
}

async function findByGeneratorSearch(
  query: string,
  trace: string[]
): Promise<CommonsCandidate | null> {
  const api = new URL("https://commons.wikimedia.org/w/api.php");
  api.searchParams.set("action", "query");
  api.searchParams.set("format", "json");
  api.searchParams.set("origin", "*");
  api.searchParams.set("generator", "search");
  api.searchParams.set("gsrnamespace", "6");
  api.searchParams.set("gsrlimit", "12");
  api.searchParams.set("gsrsearch", query);
  api.searchParams.set("prop", "imageinfo");
  api.searchParams.set("iiprop", "url");

  const resp = await sourceFetch(api.toString());
  if (!resp.ok) {
    trace.push(`commons:generator(${query}) http:${resp.status}`);
    return null;
  }
  let parsed: CommonsSearchResponse;
  try {
    parsed = (await resp.json()) as CommonsSearchResponse;
  } catch {
    trace.push(`commons:generator(${query}) bad_json`);
    return null;
  }
  const candidate = pickCandidateFromPages(parsed.query?.pages);
  trace.push(
    candidate
      ? `commons:generator(${query}) ok`
      : `commons:generator(${query}) empty`
  );
  return candidate;
}

async function findByListSearch(query: string, trace: string[]): Promise<CommonsCandidate | null> {
  const searchApi = new URL("https://commons.wikimedia.org/w/api.php");
  searchApi.searchParams.set("action", "query");
  searchApi.searchParams.set("format", "json");
  searchApi.searchParams.set("origin", "*");
  searchApi.searchParams.set("list", "search");
  searchApi.searchParams.set("srnamespace", "6");
  searchApi.searchParams.set("srlimit", "8");
  searchApi.searchParams.set("srsearch", query);

  const searchResp = await sourceFetch(searchApi.toString());
  if (!searchResp.ok) {
    trace.push(`commons:list(${query}) http:${searchResp.status}`);
    return null;
  }
  let searchParsed: CommonsListSearchResponse;
  try {
    searchParsed = (await searchResp.json()) as CommonsListSearchResponse;
  } catch {
    trace.push(`commons:list(${query}) bad_json`);
    return null;
  }
  const titles = (searchParsed.query?.search || [])
    .map((s) => (s.title || "").trim())
    .filter((t) => t.length > 0);
  if (titles.length === 0) {
    trace.push(`commons:list(${query}) no_titles`);
    return null;
  }

  const infoApi = new URL("https://commons.wikimedia.org/w/api.php");
  infoApi.searchParams.set("action", "query");
  infoApi.searchParams.set("format", "json");
  infoApi.searchParams.set("origin", "*");
  infoApi.searchParams.set("prop", "imageinfo");
  infoApi.searchParams.set("iiprop", "url");
  infoApi.searchParams.set("titles", titles.slice(0, 8).join("|"));

  const infoResp = await sourceFetch(infoApi.toString());
  if (!infoResp.ok) {
    trace.push(`commons:titles(${query}) http:${infoResp.status}`);
    return null;
  }
  let infoParsed: CommonsSearchResponse;
  try {
    infoParsed = (await infoResp.json()) as CommonsSearchResponse;
  } catch {
    trace.push(`commons:titles(${query}) bad_json`);
    return null;
  }
  const candidate = pickCandidateFromPages(infoParsed.query?.pages);
  trace.push(candidate ? `commons:list(${query}) ok` : `commons:list(${query}) empty`);
  return candidate;
}

function pickCandidateFromPages(pages: CommonsPages | undefined): CommonsCandidate | null {
  if (!pages) return null;
  for (const page of Object.values(pages)) {
    const title = (page.title || "").trim();
    const sourceUrl = (page.imageinfo?.[0]?.url || "").trim();
    if (!title || !sourceUrl) continue;
    if (!isRasterImage(sourceUrl)) continue;
    return { title, sourceUrl };
  }
  return null;
}

function isRasterImage(url: string): boolean {
  const u = url.toLowerCase();
  return (
    u.includes(".jpg") ||
    u.includes(".jpeg") ||
    u.includes(".png") ||
    u.includes(".webp")
  );
}

async function findByWikipediaThumbnail(
  query: string,
  trace: string[]
): Promise<CommonsCandidate | null> {
  const api = new URL("https://en.wikipedia.org/w/api.php");
  api.searchParams.set("action", "query");
  api.searchParams.set("format", "json");
  api.searchParams.set("origin", "*");
  api.searchParams.set("generator", "search");
  api.searchParams.set("gsrlimit", "4");
  api.searchParams.set("gsrsearch", query);
  api.searchParams.set("prop", "pageimages");
  api.searchParams.set("pithumbsize", "640");
  api.searchParams.set("piprop", "thumbnail");

  const resp = await sourceFetch(api.toString());
  if (!resp.ok) {
    trace.push(`wiki:thumb(${query}) http:${resp.status}`);
    return null;
  }
  let parsed: WikipediaThumbResponse;
  try {
    parsed = (await resp.json()) as WikipediaThumbResponse;
  } catch {
    trace.push(`wiki:thumb(${query}) bad_json`);
    return null;
  }
  const pages = parsed.query?.pages;
  if (!pages) {
    trace.push(`wiki:thumb(${query}) no_pages`);
    return null;
  }
  for (const page of Object.values(pages)) {
    const title = (page.title || "").trim();
    const sourceUrl = (page.thumbnail?.source || "").trim();
    if (!title || !sourceUrl) continue;
    if (!isRasterImage(sourceUrl)) continue;
    trace.push(`wiki:thumb(${query}) ok`);
    return { title, sourceUrl };
  }
  trace.push(`wiki:thumb(${query}) empty`);
  return null;
}

async function findByWikipediaSummary(
  query: string,
  trace: string[]
): Promise<CommonsCandidate | null> {
  const candidates = buildTitleCandidates(query);
  for (const title of candidates) {
    const url =
      "https://en.wikipedia.org/api/rest_v1/page/summary/" +
      encodeURIComponent(title);
    const resp = await sourceFetch(url);
    if (!resp.ok) {
      trace.push(`wiki:summary(${title}) http:${resp.status}`);
      continue;
    }
    let parsed: WikipediaSummaryResponse;
    try {
      parsed = (await resp.json()) as WikipediaSummaryResponse;
    } catch {
      trace.push(`wiki:summary(${title}) bad_json`);
      continue;
    }
    const sourceUrl =
      (parsed.thumbnail?.source || "").trim() ||
      (parsed.originalimage?.source || "").trim();
    if (!sourceUrl || !isRasterImage(sourceUrl)) {
      trace.push(`wiki:summary(${title}) no_image`);
      continue;
    }
    trace.push(`wiki:summary(${title}) ok`);
    return { title: (parsed.title || title).trim(), sourceUrl };
  }
  return null;
}

function buildTitleCandidates(query: string): string[] {
  const q = query.trim();
  if (!q) return [];
  const titleCase = q
    .split(/\s+/)
    .filter((p) => p.length > 0)
    .map((p) => p.charAt(0).toUpperCase() + p.slice(1).toLowerCase())
    .join(" ");
  const out: string[] = [];
  if (titleCase.length > 0) out.push(titleCase);
  if (q.length > 0 && q !== titleCase) out.push(q);
  return out;
}

async function findByOpenverse(query: string, trace: string[]): Promise<CommonsCandidate | null> {
  const api = new URL("https://api.openverse.org/v1/images/");
  api.searchParams.set("q", query);
  api.searchParams.set("page_size", "8");
  api.searchParams.set("license_type", "all");
  const resp = await sourceFetch(api.toString());
  if (!resp.ok) {
    trace.push(`openverse(${query}) http:${resp.status}`);
    return null;
  }
  let parsed: OpenverseResponse;
  try {
    parsed = (await resp.json()) as OpenverseResponse;
  } catch {
    trace.push(`openverse(${query}) bad_json`);
    return null;
  }
  const items = parsed.results || [];
  for (const item of items) {
    const title = (item.title || "Openverse image").trim();
    const sourceUrl = (item.thumbnail || item.url || "").trim();
    if (!sourceUrl || !isRasterImage(sourceUrl)) continue;
    trace.push(`openverse(${query}) ok`);
    return { title, sourceUrl };
  }
  trace.push(`openverse(${query}) empty`);
  return null;
}

async function fetchOptimizedImage(sourceUrl: string): Promise<Response> {
  return sourceFetch(sourceUrl, {
    method: "GET",
    cf: {
      image: {
        width: 160,
        height: 80,
        fit: "cover",
        format: "jpeg",
        quality: 64,
        metadata: "none",
      },
    },
  } as RequestInit);
}

async function sourceFetch(url: string, init?: RequestInit): Promise<Response> {
  const headers = new Headers(init?.headers || {});
  if (!headers.has("Accept")) headers.set("Accept", "application/json, image/*;q=0.9, */*;q=0.5");
  if (!headers.has("Api-User-Agent")) {
    headers.set(
      "Api-User-Agent",
      "DavidM5Stick/1.0 (public image lookup; contact: github.com/fmacpro/david-m5stickc)"
    );
  }
  if (!headers.has("User-Agent")) {
    headers.set(
      "User-Agent",
      "DavidM5Stick/1.0 (public image lookup; github.com/fmacpro/david-m5stickc)"
    );
  }
  return fetch(url, {
    ...init,
    method: init?.method || "GET",
    redirect: "follow",
    headers,
  });
}
