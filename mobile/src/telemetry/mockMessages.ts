import { DRIVE_POINT_COUNT } from './types.ts';

/**
 * Simulated PERG phone-telemetry lines for local testing without hardware.
 * Values are illustrative only — not real athlete measurements.
 */

const HEX = '0123456789abcdef';

/** Encode 0–255 samples as lowercase hex, matching the logger's `drive,` payload. */
export function encodeDriveHex(points: readonly number[]): string {
  if (points.length !== DRIVE_POINT_COUNT) {
    throw new RangeError(`drive curve must have ${DRIVE_POINT_COUNT} points`);
  }
  let hex = '';
  for (const point of points) {
    if (!Number.isInteger(point) || point < 0 || point > 255) {
      throw new RangeError('drive points must be integers in 0..255');
    }
    hex += HEX[point >> 4];
    hex += HEX[point & 0x0f];
  }
  return hex;
}

function risingCurve(): number[] {
  const points: number[] = [];
  for (let i = 0; i < DRIVE_POINT_COUNT; i += 1) {
    points.push(Math.round((255 * i) / (DRIVE_POINT_COUNT - 1)));
  }
  return points;
}

function midPeakCurve(): number[] {
  const points: number[] = [];
  const mid = (DRIVE_POINT_COUNT - 1) / 2;
  for (let i = 0; i < DRIVE_POINT_COUNT; i += 1) {
    const distance = Math.abs(i - mid) / mid;
    points.push(Math.round(255 * (1 - distance)));
  }
  return points;
}

function latePeakCurve(): number[] {
  const points: number[] = [];
  for (let i = 0; i < DRIVE_POINT_COUNT; i += 1) {
    const t = i / (DRIVE_POINT_COUNT - 1);
    // Simulated force that builds late in the drive, then eases.
    const shaped = t < 0.7 ? t / 0.7 : (1 - t) / 0.3;
    points.push(Math.round(255 * Math.max(0, Math.min(1, shaped))));
  }
  return points;
}

/** Valid `stats` lines that should parse successfully. */
export const MOCK_VALID_STATS: readonly string[] = [
  'stats,0,0.0,0.0,0.0,0.0,0,0.0,0.0',
  'stats,1,85.5,120.0,250.0,45.2,120,28.0,12.5',
  'stats,1,92.0,110.5,512.0,90.0,145,30.0,-8.0',
  'stats,0,10.0,180.0,1000.0,300.0,40,18.0,0.0',
];

/** Valid `drive` lines with 32 lowercase hex bytes (64 characters). */
export const MOCK_VALID_DRIVES: readonly string[] = [
  `drive,${encodeDriveHex(risingCurve())}`,
  `drive,${encodeDriveHex(midPeakCurve())}`,
  `drive,${encodeDriveHex(latePeakCurve())}`,
];

/** Logger sends this when the stroke curve is not ready / cleared. */
export const MOCK_EMPTY_DRIVE = 'drive,';

/** Stats lines that must be rejected by the parser. */
export const MOCK_MALFORMED_STATS: readonly string[] = [
  'stats,1,85.5,120.0,250.0,45.2,120,28.0', // missing balance
  'stats,1,85.5,120.0,250.0,45.2,120,28.0,12.5,99', // extra field
  'stats,2,85.5,120.0,250.0,45.2,120,28.0,12.5', // running not 0/1
  'stats,1,abc,120.0,250.0,45.2,120,28.0,12.5', // non-numeric
  'stats,1,-1.0,120.0,250.0,45.2,120,28.0,12.5', // negative rpm
  'stats,1,85.5,6000.0,250.0,45.2,120,28.0,12.5', // pace out of range
];

/** Drive lines that must be rejected by the parser. */
export const MOCK_MALFORMED_DRIVES: readonly string[] = [
  `drive,${'a'.repeat(62)}`, // too short
  `drive,${'a'.repeat(66)}`, // too long
  `drive,${'A'.repeat(64)}`, // uppercase hex
  `drive,${'g'.repeat(64)}`, // non-hex
];

/**
 * Short simulated workout as wire lines (no trailing newlines).
 * Order: stopped → start → live stats/curves → cleared curve → stop.
 */
export const MOCK_WORKOUT_SEQUENCE: readonly string[] = [
  'stats,0,0.0,0.0,0.0,0.0,0,0.0,0.0',
  MOCK_EMPTY_DRIVE,
  'stats,1,40.0,150.0,5.0,1.0,55,22.0,5.0',
  `drive,${encodeDriveHex(risingCurve())}`,
  'stats,1,78.5,125.0,45.0,12.0,110,26.0,10.0',
  `drive,${encodeDriveHex(midPeakCurve())}`,
  'stats,1,88.0,118.0,120.0,30.0,135,28.0,-4.5',
  `drive,${encodeDriveHex(latePeakCurve())}`,
  'stats,1,90.0,115.0,180.0,45.0,140,29.0,2.0',
  MOCK_EMPTY_DRIVE,
  'stats,1,70.0,130.0,200.0,52.0,95,24.0,0.0',
  'stats,0,12.0,0.0,200.0,55.0,0,0.0,0.0',
];
