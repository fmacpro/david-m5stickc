export function proxyJson(status: number, bodyText: string): Response {
  try {
    const obj = JSON.parse(bodyText) as unknown;
    return json(obj, status);
  } catch {
    return json({ error: bodyText || "Upstream error" }, status);
  }
}

export function json(data: unknown, status = 200): Response {
  return new Response(JSON.stringify(data), {
    status,
    headers: {
      "Content-Type": "application/json",
      ...corsHeaders(),
    },
  });
}

export function withCors(resp: Response): Response {
  const h = new Headers(resp.headers);
  for (const [k, v] of Object.entries(corsHeaders())) h.set(k, v);
  return new Response(resp.body, {
    status: resp.status,
    statusText: resp.statusText,
    headers: h,
  });
}

export function corsHeaders(): Record<string, string> {
  return {
    "Access-Control-Allow-Origin": "*",
    "Access-Control-Allow-Headers":
      "content-type,x-device-id,x-timestamp,x-nonce,x-signature",
    "Access-Control-Allow-Methods": "POST,OPTIONS",
  };
}
