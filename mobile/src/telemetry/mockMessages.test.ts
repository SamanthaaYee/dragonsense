import assert from 'node:assert/strict';
import { describe, it } from 'node:test';

import { LineFramer } from './lineFramer.ts';
import {
  MOCK_EMPTY_DRIVE,
  MOCK_MALFORMED_DRIVES,
  MOCK_MALFORMED_STATS,
  MOCK_VALID_DRIVES,
  MOCK_VALID_STATS,
  MOCK_WORKOUT_SEQUENCE,
} from './mockMessages.ts';
import { parseTelemetry } from './parseTelemetry.ts';
import { DRIVE_POINT_COUNT } from './types.ts';

describe('mockMessages catalog', () => {
  it('parses every valid stats message', () => {
    for (const line of MOCK_VALID_STATS) {
      const result = parseTelemetry(line);
      assert.equal(result.ok, true, `expected success for ${line}`);
      if (!result.ok) continue;
      assert.equal(result.message.type, 'stats');
    }
  });

  it('parses every valid drive message into 32 points', () => {
    for (const line of MOCK_VALID_DRIVES) {
      const result = parseTelemetry(line);
      assert.equal(result.ok, true, `expected success for ${line}`);
      if (!result.ok) continue;
      assert.equal(result.message.type, 'drive');
      if (result.message.type !== 'drive') continue;
      assert.equal(result.message.points.length, DRIVE_POINT_COUNT);
    }
  });

  it('parses the empty drive as a cleared curve', () => {
    const result = parseTelemetry(MOCK_EMPTY_DRIVE);
    assert.equal(result.ok, true);
    if (!result.ok) return;
    assert.deepEqual(result.message, { type: 'drive', points: [] });
  });

  it('rejects every malformed stats message', () => {
    for (const line of MOCK_MALFORMED_STATS) {
      assert.equal(parseTelemetry(line).ok, false, `expected failure for ${line}`);
    }
  });

  it('rejects every malformed drive message', () => {
    for (const line of MOCK_MALFORMED_DRIVES) {
      assert.equal(parseTelemetry(line).ok, false, `expected failure for ${line}`);
    }
  });
});

describe('mock workout sequence through LineFramer + parseTelemetry', () => {
  it('frames the joined stream and parses each line', () => {
    const framer = new LineFramer();
    const stream = `${MOCK_WORKOUT_SEQUENCE.join('\n')}\n`;

    // Feed in awkward chunk sizes to exercise buffering, not just whole lines.
    const chunkSize = 17;
    const framed: string[] = [];
    for (let offset = 0; offset < stream.length; offset += chunkSize) {
      framed.push(...framer.push(stream.slice(offset, offset + chunkSize)));
    }

    assert.deepEqual(framed, [...MOCK_WORKOUT_SEQUENCE]);

    for (const line of framed) {
      const result = parseTelemetry(line);
      assert.equal(result.ok, true, `workout line failed: ${line}`);
    }
  });

  it('includes stopped, running, drive curves, and a cleared curve', () => {
    const parsed = MOCK_WORKOUT_SEQUENCE.map((line) => parseTelemetry(line));
    assert.ok(parsed.every((result) => result.ok));

    const messages = parsed.flatMap((result) => (result.ok ? [result.message] : []));
    const stats = messages.filter((message) => message.type === 'stats');
    const drives = messages.filter((message) => message.type === 'drive');

    assert.ok(stats.some((message) => message.type === 'stats' && !message.running));
    assert.ok(stats.some((message) => message.type === 'stats' && message.running));
    assert.ok(drives.some((message) => message.type === 'drive' && message.points.length === 0));
    assert.ok(
      drives.some(
        (message) => message.type === 'drive' && message.points.length === DRIVE_POINT_COUNT,
      ),
    );
  });
});
