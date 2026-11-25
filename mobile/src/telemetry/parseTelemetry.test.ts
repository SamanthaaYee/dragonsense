import assert from 'node:assert/strict';
import { describe, it } from 'node:test';

import { parseTelemetry } from './parseTelemetry.ts';
import { DRIVE_POINT_COUNT } from './types.ts';

const VALID_DRIVE_HEX =
  '000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f';

describe('parseTelemetry stats', () => {
  it('parses a valid running stats message', () => {
    const result = parseTelemetry('stats,1,85.5,2.10,250,45.2,120,28,52.3');
    assert.equal(result.ok, true);
    if (!result.ok) return;
    assert.deepEqual(result.message, {
      type: 'stats',
      running: true,
      rpm: 85.5,
      pace: 2.1,
      distance: 250,
      elapsed: 45.2,
      watts: 120,
      spm: 28,
      balance: 52.3,
    });
  });

  it('parses a stopped workout (running = 0)', () => {
    const result = parseTelemetry('stats,0,0.0,0.0,0.0,0.0,0,0.0,0.0');
    assert.equal(result.ok, true);
    if (!result.ok) return;
    assert.equal(result.message.type, 'stats');
    if (result.message.type !== 'stats') return;
    assert.equal(result.message.running, false);
  });

  it('rejects missing fields', () => {
    const result = parseTelemetry('stats,1,85.5,2.10,250,45.2,120,28');
    assert.equal(result.ok, false);
  });

  it('rejects extra fields', () => {
    const result = parseTelemetry('stats,1,85.5,2.10,250,45.2,120,28,52.3,99');
    assert.equal(result.ok, false);
  });

  it('rejects invalid numeric values', () => {
    assert.equal(parseTelemetry('stats,1,abc,2.10,250,45.2,120,28,52.3').ok, false);
    assert.equal(parseTelemetry('stats,1,,2.10,250,45.2,120,28,52.3').ok, false);
    assert.equal(parseTelemetry('stats,1,-1,2.10,250,45.2,120,28,52.3').ok, false);
    assert.equal(parseTelemetry('stats,1,85.5,6000,250,45.2,120,28,52.3').ok, false);
    assert.equal(parseTelemetry('stats,1,85.5,2.10,250,45.2,120,28,100.1').ok, false);
    assert.equal(parseTelemetry('stats,2,85.5,2.10,250,45.2,120,28,52.3').ok, false);
  });
});

describe('parseTelemetry drive', () => {
  it('parses a valid drive message into 32 points', () => {
    const result = parseTelemetry(`drive,${VALID_DRIVE_HEX}`);
    assert.equal(result.ok, true);
    if (!result.ok) return;
    assert.equal(result.message.type, 'drive');
    if (result.message.type !== 'drive') return;
    assert.equal(result.message.points.length, DRIVE_POINT_COUNT);
    assert.deepEqual(result.message.points.slice(0, 4), [0, 1, 2, 3]);
    assert.equal(result.message.points[31], 0x1f);
  });

  it('accepts an empty drive payload as a cleared curve', () => {
    const result = parseTelemetry('drive,');
    assert.equal(result.ok, true);
    if (!result.ok) return;
    assert.deepEqual(result.message, { type: 'drive', points: [] });
  });

  it('rejects invalid hexadecimal characters', () => {
    const withUpper = `drive,${'A'.repeat(64)}`;
    const withGarbage = `drive,${'g'.repeat(64)}`;
    assert.equal(parseTelemetry(withUpper).ok, false);
    assert.equal(parseTelemetry(withGarbage).ok, false);
  });

  it('rejects too few or too many hex characters', () => {
    assert.equal(parseTelemetry(`drive,${VALID_DRIVE_HEX.slice(0, 62)}`).ok, false);
    assert.equal(parseTelemetry(`drive,${VALID_DRIVE_HEX}00`).ok, false);
  });
});

describe('parseTelemetry unsupported', () => {
  it('rejects unrecognized prefixes without throwing', () => {
    assert.equal(parseTelemetry('session_state,1').ok, false);
    assert.equal(parseTelemetry('').ok, false);
    assert.equal(parseTelemetry('stats').ok, false);
  });
});
