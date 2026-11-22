// Workout line encoding and validation for the Perg display.
// Mirrors firmware/perg_freenove_esp32_s3_display/Workout.h; the format is
// specified in docs/workout_push.md. No Bluetooth code belongs here, so the
// same encoding serves any transport. Works in browsers (global PergWorkout)
// and in Node (module.exports) for tests.
(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) {
    module.exports = api;
  } else {
    root.PergWorkout = api;
  }
})(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  const LIMITS = Object.freeze({
    maxPieces: 20,
    minTimeS: 10,
    maxTimeS: 3600,
    minDistanceM: 50,
    maxDistanceM: 10000,
    maxRestS: 1800,
    maxLine: 2 + 20 * 12, // "W1" + 20 x ";D10000,1800"
  });
  const VERSION = 'W1';

  // ---------------- Input helpers ----------------

  // "90" or "1:30" -> 90 seconds; null when not a whole-second duration.
  function parseDuration(text) {
    const value = String(text).trim();
    const seconds = /^(\d{1,3})$/.exec(value);
    if (seconds) return Number(seconds[1]);
    const clock = /^(\d{1,2}):([0-5]\d)$/.exec(value);
    if (clock) return Number(clock[1]) * 60 + Number(clock[2]);
    return null;
  }

  // Whole metres only.
  function parseDistance(text) {
    const value = String(text).trim();
    return /^\d{1,5}$/.test(value) ? Number(value) : null;
  }

  function formatDuration(seconds) {
    const minutes = Math.floor(seconds / 60);
    return `${minutes}:${String(seconds % 60).padStart(2, '0')}`;
  }

  // ---------------- Encoding ----------------

  function isWhole(value) {
    return typeof value === 'number' && Number.isInteger(value);
  }

  function pieceError(piece, number) {
    const prefix = `Piece ${number}:`;
    if (!piece || (piece.kind !== 'T' && piece.kind !== 'D')) {
      return `${prefix} choose time or distance.`;
    }
    if (piece.kind === 'T') {
      if (!isWhole(piece.target)) return `${prefix} enter the time as m:ss or seconds.`;
      if (piece.target < LIMITS.minTimeS || piece.target > LIMITS.maxTimeS) {
        return `${prefix} time must be between ${formatDuration(LIMITS.minTimeS)} and ` +
          `${formatDuration(LIMITS.maxTimeS)}.`;
      }
    } else {
      if (!isWhole(piece.target)) return `${prefix} enter the distance in whole metres.`;
      if (piece.target < LIMITS.minDistanceM || piece.target > LIMITS.maxDistanceM) {
        return `${prefix} distance must be between ${LIMITS.minDistanceM} and ` +
          `${LIMITS.maxDistanceM} m.`;
      }
    }
    if (!isWhole(piece.rest)) return `${prefix} enter the rest as m:ss or seconds.`;
    if (piece.rest < 0 || piece.rest > LIMITS.maxRestS) {
      return `${prefix} rest must be between 0:00 and ${formatDuration(LIMITS.maxRestS)}.`;
    }
    return null;
  }

  // pieces: [{ kind: 'T' | 'D', target, rest }] -> { line, error }.
  // An empty list encodes "W1", which clears the display's workout.
  function encode(pieces) {
    if (!Array.isArray(pieces)) return { line: null, error: 'Pieces must be a list.' };
    if (pieces.length > LIMITS.maxPieces) {
      return { line: null, error: `A workout can have at most ${LIMITS.maxPieces} pieces.` };
    }
    for (let i = 0; i < pieces.length; i += 1) {
      const error = pieceError(pieces[i], i + 1);
      if (error) return { line: null, error };
    }
    const body = pieces.map((p) => `;${p.kind}${p.target},${p.rest}`).join('');
    return { line: VERSION + body, error: null };
  }

  // ---------------- Decoding (same rules and error codes as the display) ----------------

  function readNumber(line, position) {
    const match = /\d+/y;
    match.lastIndex = position;
    const found = match.exec(line);
    if (!found) return null;
    const digits = found[0];
    if (digits.length > 5 || (digits.length > 1 && digits[0] === '0')) return null;
    return { value: Number(digits), next: position + digits.length };
  }

  function inRange(kind, target, rest) {
    const targetOk = kind === 'T'
      ? target >= LIMITS.minTimeS && target <= LIMITS.maxTimeS
      : target >= LIMITS.minDistanceM && target <= LIMITS.maxDistanceM;
    return targetOk && rest <= LIMITS.maxRestS;
  }

  // -> { error: 'none' | 'length' | 'version' | 'syntax' | 'range' | 'count', pieces }
  function decode(line) {
    const fail = (error) => ({ error, pieces: [] });
    if (line.length > LIMITS.maxLine) return fail('length');
    if (line[0] !== 'W' || line[1] !== '1') return fail('version');
    const pieces = [];
    let p = 2;
    while (p < line.length) {
      if (line[p] !== ';') return fail('syntax');
      const kind = line[p + 1];
      if (kind !== 'T' && kind !== 'D') return fail('syntax');
      const target = readNumber(line, p + 2);
      if (!target || line[target.next] !== ',') return fail('syntax');
      const rest = readNumber(line, target.next + 1);
      if (!rest) return fail('syntax');
      if (!inRange(kind, target.value, rest.value)) return fail('range');
      if (pieces.length === LIMITS.maxPieces) return fail('count');
      pieces.push({ kind, target: target.value, rest: rest.value });
      p = rest.next;
    }
    return { error: 'none', pieces };
  }

  // ---------------- Acknowledgment ----------------

  // FNV-1a 32-bit over the line's bytes, as 8 lowercase hex digits.
  function checksum(line) {
    let hash = 0x811c9dc5;
    for (let i = 0; i < line.length; i += 1) {
      hash ^= line.charCodeAt(i) & 0xff;
      hash = Math.imul(hash, 0x01000193) >>> 0;
    }
    return hash.toString(16).padStart(8, '0');
  }

  function parseStatus(value) {
    const ok = /^OK,(\d{1,2}),([0-9a-f]{8})$/.exec(value);
    if (ok) return { state: 'ok', count: Number(ok[1]), hash: ok[2] };
    const error = /^ERR,([a-z]+)$/.exec(value);
    if (error) return { state: 'error', code: error[1] };
    if (value === 'RECV' || value === 'READY') return { state: 'pending' };
    return { state: 'unknown', raw: value };
  }

  // True only when the display confirmed exactly this line.
  function confirms(line, statusValue) {
    const status = parseStatus(statusValue);
    return status.state === 'ok' &&
      status.hash === checksum(line) &&
      status.count === decode(line).pieces.length;
  }

  function describeDisplayError(code) {
    if (code === 'busy') {
      return 'The Perg is in a session. Stop it on the display, then send again.';
    }
    return `The Perg rejected the workout (${code}). ` +
      'Make sure the page and the display firmware are up to date.';
  }

  return Object.freeze({
    LIMITS,
    parseDuration,
    parseDistance,
    formatDuration,
    encode,
    decode,
    checksum,
    parseStatus,
    confirms,
    describeDisplayError,
  });
});
