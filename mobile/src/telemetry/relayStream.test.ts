import assert from 'node:assert/strict';
import { Buffer } from 'buffer';
import { describe, it } from 'node:test';

import { parseTelemetry } from './parseTelemetry.ts';
import { formatCapture, RelayStream } from './relayStream.ts';

const STATS = 'stats,1,85.5,120.5,250.0,45.2,120,28.0,-5.0,11.1,22.2,33.3,44.4';
const PEAK = 'foot_peak,33.3,77.7';

describe('current Freenove relay', () => {
  it('decodes all four loads in the logger’s wire order', () => {
    const result = parseTelemetry(STATS);
    assert.equal(result.ok, true);
    if (!result.ok || result.message.type !== 'stats') return;
    assert.deepEqual(result.message.footLoads, {
      leftBottom: 11.1,
      leftTop: 22.2,
      rightTop: 33.3,
      rightBottom: 44.4,
    });
    assert.equal(parseTelemetry(STATS.replace('11.1', '-1')).ok, false);
    assert.equal(parseTelemetry(STATS.replace('44.4', 'NaN')).ok, false);
  });

  it('decodes a simultaneous peak and the peak reset', () => {
    assert.deepEqual(parseTelemetry(PEAK), {
      ok: true,
      message: { type: 'foot_peak', loads: { left: 33.3, right: 77.7 } },
    });
    assert.deepEqual(parseTelemetry('foot_peak'), {
      ok: true,
      message: { type: 'foot_peak', loads: null },
    });
    assert.equal(parseTelemetry('foot_peak,33.3,-1').ok, false);
  });

  it('reassembles base64 notification fragments and saves timestamped records', () => {
    const stream = new RelayStream();
    const drive = 'drive,' + '00ff'.repeat(16);
    const input = [STATS, drive, PEAK, 'foot_peak', 'bad line'].join('\n') + '\n';
    const rows = [];
    for (let index = 0; index < input.length; index += 20) {
      const chunk = Buffer.from(input.slice(index, index + 20)).toString('base64');
      rows.push(...stream.push(chunk, 1000 + index));
    }
    assert.deepEqual(rows.map((row) => row.line), [STATS, drive, PEAK, 'foot_peak']);
    assert.deepEqual(rows.map((row) => row.message.type), ['stats', 'drive', 'foot_peak', 'foot_peak']);
    assert.equal(rows[0].receivedAtMs, 1060);
    const saved = formatCapture(rows).trimEnd().split('\n').map((row) => JSON.parse(row));
    assert.deepEqual(saved, rows);
  });
});
