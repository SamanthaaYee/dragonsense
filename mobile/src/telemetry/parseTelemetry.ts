import {
  DRIVE_POINT_COUNT,
  type DriveTelemetry,
  type ParseTelemetryResult,
  type StatsTelemetry,
} from './types.ts';

/** Matches display `readStats` / logger format limits. */
const MAX_PACE = 5999;
const MAX_ELAPSED = 4_294_967;
const HEX_DIGITS = '0123456789abcdef';

function fail(error: string): ParseTelemetryResult {
  return { ok: false, error };
}

function parseFiniteNumber(raw: string): number | null {
  // Reject empty fields and any whitespace so "" does not become 0.
  if (raw.length === 0 || /\s/.test(raw)) return null;
  const value = Number(raw);
  if (!Number.isFinite(value)) return null;
  return value;
}

function parseStats(fields: string[]): ParseTelemetryResult {
  // stats + 8 values
  if (fields.length !== 9) {
    return fail(`stats expects 9 fields, got ${fields.length}`);
  }

  const runningRaw = fields[1];
  if (runningRaw !== '0' && runningRaw !== '1') {
    return fail('stats running must be 0 or 1');
  }

  const rpm = parseFiniteNumber(fields[2]);
  const pace = parseFiniteNumber(fields[3]);
  const distance = parseFiniteNumber(fields[4]);
  const elapsed = parseFiniteNumber(fields[5]);
  const watts = parseFiniteNumber(fields[6]);
  const spm = parseFiniteNumber(fields[7]);
  const balance = parseFiniteNumber(fields[8]);

  if (
    rpm === null ||
    pace === null ||
    distance === null ||
    elapsed === null ||
    watts === null ||
    spm === null ||
    balance === null
  ) {
    return fail('stats contains a non-numeric field');
  }

  if (rpm < 0 || pace < 0 || distance < 0 || elapsed < 0 || watts < 0 || spm < 0) {
    return fail('stats numeric fields must be non-negative');
  }
  if (pace > MAX_PACE) return fail('stats pace is out of range');
  if (elapsed > MAX_ELAPSED) return fail('stats elapsed is out of range');
  if (balance < -100 || balance > 100) return fail('stats balance is out of range');

  const message: StatsTelemetry = {
    type: 'stats',
    running: runningRaw === '1',
    rpm,
    pace,
    distance,
    elapsed,
    watts,
    spm,
    balance,
  };
  return { ok: true, message };
}

function hexValue(char: string): number | null {
  const index = HEX_DIGITS.indexOf(char);
  return index >= 0 ? index : null;
}

function parseDrive(payload: string): ParseTelemetryResult {
  // Logger sends `drive,` with an empty payload when the curve is not ready.
  if (payload.length === 0) {
    const message: DriveTelemetry = { type: 'drive', points: [] };
    return { ok: true, message };
  }

  const expectedLength = DRIVE_POINT_COUNT * 2;
  if (payload.length !== expectedLength) {
    return fail(
      `drive expects ${expectedLength} hex characters or empty, got ${payload.length}`,
    );
  }

  const points: number[] = [];
  for (let i = 0; i < DRIVE_POINT_COUNT; i += 1) {
    const high = hexValue(payload[2 * i]);
    const low = hexValue(payload[2 * i + 1]);
    if (high === null || low === null) {
      return fail('drive payload must be lowercase hexadecimal');
    }
    points.push((high << 4) | low);
  }

  const message: DriveTelemetry = { type: 'drive', points };
  return { ok: true, message };
}

/**
 * Parse one complete telemetry line (no trailing newline).
 * Returns a structured message or a non-throwing failure result.
 */
export function parseTelemetry(line: string): ParseTelemetryResult {
  const trimmed = line.trim();
  if (trimmed.length === 0) return fail('empty message');

  const comma = trimmed.indexOf(',');
  if (comma < 0) return fail('unsupported message');

  const kind = trimmed.slice(0, comma);
  const rest = trimmed.slice(comma + 1);

  if (kind === 'stats') {
    // Keep empty trailing fields so missing values are visible as "".
    return parseStats(trimmed.split(','));
  }
  if (kind === 'drive') {
    // Do not split the hex payload on commas; the format is drive,<hex>.
    return parseDrive(rest);
  }
  return fail(`unsupported message type: ${kind}`);
}
