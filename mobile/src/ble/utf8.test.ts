import assert from 'node:assert/strict';
import { describe, it } from 'node:test';

import { decodeBase64Utf8 } from './utf8.ts';

function encodeBase64(text: string): string {
  const bytes = new TextEncoder().encode(text);
  let binary = '';
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary);
}

describe('decodeBase64Utf8', () => {
  it('decodes a UTF-8 telemetry chunk', () => {
    const payload = 'stats,1,85.5,120.0,250.0,45.2,120,28.0,12.5\n';
    const result = decodeBase64Utf8(encodeBase64(payload));
    assert.equal(result.ok, true);
    if (!result.ok) return;
    assert.equal(result.text, payload);
  });

  it('decodes an empty drive line', () => {
    const result = decodeBase64Utf8(encodeBase64('drive,\n'));
    assert.equal(result.ok, true);
    if (!result.ok) return;
    assert.equal(result.text, 'drive,\n');
  });

  it('allows whitespace inside the base64 string', () => {
    const encoded = encodeBase64('stats,0,0,0,0,0,0,0,0\n');
    const spaced = `${encoded.slice(0, 8)}\n${encoded.slice(8)}`;
    const result = decodeBase64Utf8(spaced);
    assert.equal(result.ok, true);
    if (!result.ok) return;
    assert.equal(result.text, 'stats,0,0,0,0,0,0,0,0\n');
  });

  it('rejects empty input', () => {
    assert.equal(decodeBase64Utf8('').ok, false);
    assert.equal(decodeBase64Utf8('   ').ok, false);
  });

  it('rejects invalid base64 characters', () => {
    const result = decodeBase64Utf8('!!!!');
    assert.equal(result.ok, false);
  });

  it('rejects invalid base64 length', () => {
    const result = decodeBase64Utf8('abc');
    assert.equal(result.ok, false);
  });

  it('rejects invalid UTF-8 byte sequences', () => {
    // 0xff is invalid as a leading UTF-8 byte.
    const result = decodeBase64Utf8('/w==');
    assert.equal(result.ok, false);
    if (result.ok) return;
    assert.ok(result.error.includes('UTF-8'));
  });
});
