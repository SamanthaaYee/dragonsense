import assert from 'node:assert/strict';
import { describe, it } from 'node:test';

import { LineFramer, TELEMETRY_MAX_LINE_LENGTH } from './lineFramer.ts';

describe('LineFramer', () => {
  it('returns one complete message', () => {
    const framer = new LineFramer();
    assert.deepEqual(framer.push('stats,1,0,0,0,0,0,0,0\n'), [
      'stats,1,0,0,0,0,0,0,0',
    ]);
  });

  it('returns multiple messages from one chunk', () => {
    const framer = new LineFramer();
    assert.deepEqual(framer.push('stats,1,0,0,0,0,0,0,0\ndrive,\n'), [
      'stats,1,0,0,0,0,0,0,0',
      'drive,',
    ]);
  });

  it('assembles a message split across two chunks', () => {
    const framer = new LineFramer();
    assert.deepEqual(framer.push('stats,1,85.5,'), []);
    assert.deepEqual(framer.push('2.1,250,45.2,120,28,52.3\n'), [
      'stats,1,85.5,2.1,250,45.2,120,28,52.3',
    ]);
  });

  it('assembles a message split across several chunks', () => {
    const framer = new LineFramer();
    assert.deepEqual(framer.push('dri'), []);
    assert.deepEqual(framer.push('ve,'), []);
    assert.deepEqual(framer.push('ab'), []);
    assert.deepEqual(framer.push('cd\n'), ['drive,abcd']);
  });

  it('ignores empty chunks', () => {
    const framer = new LineFramer();
    assert.deepEqual(framer.push(''), []);
    assert.deepEqual(framer.push('stats,0,0,0,0,0,0,0,0\n'), [
      'stats,0,0,0,0,0,0,0,0',
    ]);
  });

  it('strips CRLF carriage returns', () => {
    const framer = new LineFramer();
    assert.deepEqual(framer.push('drive,\r\n'), ['drive,']);
    assert.deepEqual(framer.push('stats,1,0,0,0,0,0,0,0\r\n'), [
      'stats,1,0,0,0,0,0,0,0',
    ]);
  });

  it('ignores empty lines', () => {
    const framer = new LineFramer();
    assert.deepEqual(framer.push('\n\nstats,1,0,0,0,0,0,0,0\n\n'), [
      'stats,1,0,0,0,0,0,0,0',
    ]);
  });

  it('clears a partial buffer on reset', () => {
    const framer = new LineFramer();
    assert.deepEqual(framer.push('stats,1,partial'), []);
    framer.reset();
    assert.deepEqual(framer.push('drive,\n'), ['drive,']);
  });

  it('drops an oversized incomplete message at its newline', () => {
    const framer = new LineFramer();
    const oversized = 'x'.repeat(TELEMETRY_MAX_LINE_LENGTH + 1);
    assert.deepEqual(framer.push(oversized), []);
    assert.deepEqual(framer.push('\n'), []);
    assert.deepEqual(framer.push('drive,\n'), ['drive,']);
  });

  it('drops an oversized message contained in one chunk', () => {
    const framer = new LineFramer();
    const oversized = `${'y'.repeat(TELEMETRY_MAX_LINE_LENGTH + 20)}\nokay\n`;
    assert.deepEqual(framer.push(oversized), ['okay']);
  });
});
