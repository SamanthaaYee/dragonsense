export type DecodeBase64Utf8Result =
  | { ok: true; text: string }
  | { ok: false; error: string };

const BASE64_ALPHABET =
  'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

function fail(error: string): DecodeBase64Utf8Result {
  return { ok: false, error };
}

/**
 * Decode a base64 BLE notification payload into a UTF-8 string.
 * Returns a non-throwing result so callers can ignore bad chunks safely.
 */
export function decodeBase64Utf8(base64: string): DecodeBase64Utf8Result {
  if (typeof base64 !== 'string') return fail('payload must be a string');

  const cleaned = base64.replace(/\s+/g, '');
  if (cleaned.length === 0) return fail('empty base64 payload');
  if (cleaned.length % 4 !== 0) return fail('invalid base64 length');
  if (!/^[A-Za-z0-9+/]+={0,2}$/.test(cleaned)) return fail('invalid base64 characters');

  const padding = cleaned.endsWith('==') ? 2 : cleaned.endsWith('=') ? 1 : 0;
  const outputLength = (cleaned.length / 4) * 3 - padding;
  const bytes = new Uint8Array(outputLength);

  let outputIndex = 0;
  for (let i = 0; i < cleaned.length; i += 4) {
    const c0 = BASE64_ALPHABET.indexOf(cleaned[i]);
    const c1 = BASE64_ALPHABET.indexOf(cleaned[i + 1]);
    const c2 = cleaned[i + 2] === '=' ? 0 : BASE64_ALPHABET.indexOf(cleaned[i + 2]);
    const c3 = cleaned[i + 3] === '=' ? 0 : BASE64_ALPHABET.indexOf(cleaned[i + 3]);

    if (c0 < 0 || c1 < 0 || c2 < 0 || c3 < 0) return fail('invalid base64 characters');

    const triple = (c0 << 18) | (c1 << 12) | (c2 << 6) | c3;
    if (outputIndex < outputLength) bytes[outputIndex++] = (triple >> 16) & 0xff;
    if (outputIndex < outputLength) bytes[outputIndex++] = (triple >> 8) & 0xff;
    if (outputIndex < outputLength) bytes[outputIndex++] = triple & 0xff;
  }

  try {
    const text = new TextDecoder('utf-8', { fatal: true }).decode(bytes);
    return { ok: true, text };
  } catch {
    return fail('invalid UTF-8 payload');
  }
}
