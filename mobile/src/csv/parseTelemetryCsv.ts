import type { ParseTelemetryCsvResult, TelemetryCsvRow } from './types.ts';

const REQUIRED_COLUMNS = [
  'time_us',
  'rpm',
  'pace_s',
  'distance_m',
  'hall_v',
  'period_ms',
  'elapsed_s',
  'watts',
  'stroke_rate_spm',
] as const;

type RequiredColumn = (typeof REQUIRED_COLUMNS)[number];

function fail(error: string): ParseTelemetryCsvResult {
  return { ok: false, error };
}

function splitCsvLine(line: string): string[] {
  return line.split(',');
}

function parseFiniteNumber(raw: string): number | null {
  if (raw.length === 0 || /\s/.test(raw)) return null;
  const value = Number(raw);
  if (!Number.isFinite(value)) return null;
  return value;
}

function parseDataRow(
  fields: string[],
  columnIndex: ReadonlyMap<RequiredColumn, number>,
): TelemetryCsvRow | null {
  const values = {} as Record<RequiredColumn, number>;

  for (const column of REQUIRED_COLUMNS) {
    const index = columnIndex.get(column);
    if (index === undefined || index >= fields.length) return null;
    const parsed = parseFiniteNumber(fields[index]);
    if (parsed === null) return null;
    values[column] = parsed;
  }

  // hall_v may be negative; every other required field must be >= 0.
  for (const column of REQUIRED_COLUMNS) {
    if (column === 'hall_v') continue;
    if (values[column] < 0) return null;
  }

  return values;
}

/**
 * Parse a `perg_telemetry.csv` document and return the last valid data row.
 * Column order may vary; unknown columns are ignored.
 */
export function parseTelemetryCsv(csvText: string): ParseTelemetryCsvResult {
  const normalized = csvText.replace(/\r\n/g, '\n').replace(/\r/g, '\n');
  const lines = normalized.split('\n').filter((line) => line.trim().length > 0);

  if (lines.length === 0) return fail('empty file');

  const headerFields = splitCsvLine(lines[0].trim());
  const columnIndex = new Map<RequiredColumn, number>();

  for (let i = 0; i < headerFields.length; i += 1) {
    const name = headerFields[i].trim();
    if ((REQUIRED_COLUMNS as readonly string[]).includes(name)) {
      // First occurrence wins if a name is duplicated.
      if (!columnIndex.has(name as RequiredColumn)) {
        columnIndex.set(name as RequiredColumn, i);
      }
    }
  }

  const missing = REQUIRED_COLUMNS.filter((column) => !columnIndex.has(column));
  if (missing.length > 0) {
    return fail(`missing required columns: ${missing.join(', ')}`);
  }

  if (lines.length < 2) return fail('no data rows');

  for (let lineIndex = lines.length - 1; lineIndex >= 1; lineIndex -= 1) {
    const fields = splitCsvLine(lines[lineIndex].trim());
    const row = parseDataRow(fields, columnIndex);
    if (row) return { ok: true, row };
  }

  return fail('no valid data rows');
}
