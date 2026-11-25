import assert from 'node:assert/strict';
import { describe, it } from 'node:test';

import { parseTelemetryCsv } from './parseTelemetryCsv.ts';

const HEADER =
  'time_us,rpm,pace_s,distance_m,hall_v,period_ms,elapsed_s,watts,stroke_rate_spm';

describe('parseTelemetryCsv', () => {
  it('parses a valid CSV and returns the final data row', () => {
    const csv = [
      HEADER,
      '1000000,120,180,50,-0.5,100,30,150,45',
      '2000000,130,170,60,-0.2,95,40,160,46',
    ].join('\n');

    const result = parseTelemetryCsv(csv);
    assert.equal(result.ok, true);
    if (!result.ok) return;
    assert.deepEqual(result.row, {
      time_us: 2_000_000,
      rpm: 130,
      pace_s: 170,
      distance_m: 60,
      hall_v: -0.2,
      period_ms: 95,
      elapsed_s: 40,
      watts: 160,
      stroke_rate_spm: 46,
    });
  });

  it('extracts the final valid row rather than the first', () => {
    const csv = [
      HEADER,
      '1,10,20,1,0,1,1,1,1',
      '2,20,20,2,0,1,2,2,2',
      '3,30,20,3,0,1,3,3,3',
    ].join('\n');

    const result = parseTelemetryCsv(csv);
    assert.equal(result.ok, true);
    if (!result.ok) return;
    assert.equal(result.row.time_us, 3);
    assert.equal(result.row.rpm, 30);
  });

  it('handles reordered columns', () => {
    const csv = [
      'watts,stroke_rate_spm,time_us,rpm,pace_s,distance_m,hall_v,period_ms,elapsed_s',
      '150,45,1000000,120,180,50,-0.5,100,30',
    ].join('\n');

    const result = parseTelemetryCsv(csv);
    assert.equal(result.ok, true);
    if (!result.ok) return;
    assert.deepEqual(result.row, {
      time_us: 1_000_000,
      rpm: 120,
      pace_s: 180,
      distance_m: 50,
      hall_v: -0.5,
      period_ms: 100,
      elapsed_s: 30,
      watts: 150,
      stroke_rate_spm: 45,
    });
  });

  it('allows additional columns', () => {
    const csv = [
      'time_us,rpm,pace_s,distance_m,hall_v,period_ms,elapsed_s,watts,stroke_rate_spm,notes',
      '1000000,120,180,50,-0.5,100,30,150,45,ignore-me',
    ].join('\n');

    const result = parseTelemetryCsv(csv);
    assert.equal(result.ok, true);
    if (!result.ok) return;
    assert.equal(result.row.watts, 150);
  });

  it('handles Windows and Unix line endings', () => {
    const unix = `${HEADER}\n1000000,120,180,50,-0.5,100,30,150,45\n`;
    const windows = `${HEADER}\r\n1000000,120,180,50,-0.5,100,30,150,45\r\n`;

    const unixResult = parseTelemetryCsv(unix);
    const windowsResult = parseTelemetryCsv(windows);
    assert.equal(unixResult.ok, true);
    assert.equal(windowsResult.ok, true);
    if (!unixResult.ok || !windowsResult.ok) return;
    assert.deepEqual(unixResult.row, windowsResult.row);
  });

  it('ignores blank lines', () => {
    const csv = [
      HEADER,
      '',
      '1000000,120,180,50,-0.5,100,30,150,45',
      '',
      '2000000,130,170,60,-0.2,95,40,160,46',
      '',
    ].join('\n');

    const result = parseTelemetryCsv(csv);
    assert.equal(result.ok, true);
    if (!result.ok) return;
    assert.equal(result.row.time_us, 2_000_000);
  });

  it('skips an invalid final row and returns the previous valid row', () => {
    const csv = [
      HEADER,
      '1000000,120,180,50,-0.5,100,30,150,45',
      '2000000,130,170,60,-0.2,95,40,160', // truncated
    ].join('\n');

    const result = parseTelemetryCsv(csv);
    assert.equal(result.ok, true);
    if (!result.ok) return;
    assert.equal(result.row.time_us, 1_000_000);
    assert.equal(result.row.rpm, 120);
  });

  it('rejects missing required columns', () => {
    const csv = [
      'time_us,rpm,pace_s,distance_m,hall_v,period_ms,elapsed_s,watts',
      '1000000,120,180,50,-0.5,100,30,150',
    ].join('\n');

    const result = parseTelemetryCsv(csv);
    assert.equal(result.ok, false);
    if (result.ok) return;
    assert.ok(result.error.includes('stroke_rate_spm'));
  });

  it('rejects an empty file', () => {
    assert.equal(parseTelemetryCsv('').ok, false);
    assert.equal(parseTelemetryCsv('   \n\n').ok, false);
  });

  it('rejects a header with no data rows', () => {
    const result = parseTelemetryCsv(`${HEADER}\n`);
    assert.equal(result.ok, false);
  });

  it('rejects non-finite or invalid numeric values when no valid row remains', () => {
    const csv = [
      HEADER,
      '1000000,abc,180,50,-0.5,100,30,150,45',
      '2000000,130,NaN,60,-0.2,95,40,160,46',
    ].join('\n');

    assert.equal(parseTelemetryCsv(csv).ok, false);
  });

  it('allows negative hall_v values', () => {
    const csv = [
      HEADER,
      '1000000,120,180,50,-1.25,100,30,150,45',
    ].join('\n');

    const result = parseTelemetryCsv(csv);
    assert.equal(result.ok, true);
    if (!result.ok) return;
    assert.equal(result.row.hall_v, -1.25);
  });

  it('rejects negative values for non-hall fields', () => {
    const csv = [
      HEADER,
      '1000000,-1,180,50,-0.5,100,30,150,45',
    ].join('\n');

    assert.equal(parseTelemetryCsv(csv).ok, false);
  });
});
