import assert from 'node:assert/strict';
import { describe, it } from 'node:test';

import type { DriveTelemetry, StatsTelemetry } from '../telemetry/types.ts';
import { WorkoutRecorder } from './WorkoutRecorder.ts';
import type { WorkoutRecording } from './types.ts';

function stats(running: boolean, rpm = 80): StatsTelemetry {
  return {
    type: 'stats',
    running,
    rpm,
    pace: 120,
    distance: 10,
    elapsed: 5,
    watts: 100,
    spm: 28,
    balance: 0,
  };
}

function drive(points: number[] = []): DriveTelemetry {
  return { type: 'drive', points };
}

function createClock(start = 1000) {
  let t = start;
  return {
    now: () => t,
    advance: (ms: number) => {
      t += ms;
    },
  };
}

describe('WorkoutRecorder', () => {
  it('starts idle and does not record until running is true', () => {
    const recorder = new WorkoutRecorder({ now: () => 0 });
    assert.equal(recorder.getState(), 'idle');
    assert.equal(recorder.handleMessage(stats(false)), null);
    assert.equal(recorder.handleMessage(drive([1, 2, 3])), null);
    assert.equal(recorder.getState(), 'idle');
  });

  it('does not start recording from connection alone (no messages)', () => {
    const finalized: WorkoutRecording[] = [];
    const recorder = new WorkoutRecorder({
      now: () => 0,
      onFinalized: (recording) => finalized.push(recording),
    });
    assert.equal(recorder.getState(), 'idle');
    assert.equal(finalized.length, 0);
  });

  it('starts recording on stats with running === true', () => {
    const clock = createClock();
    const recorder = new WorkoutRecorder({ now: clock.now });
    assert.equal(recorder.handleMessage(stats(true, 70)), null);
    assert.equal(recorder.getState(), 'recording');
  });

  it('captures stats and drive messages in arrival order', () => {
    const clock = createClock(500);
    const finalized: WorkoutRecording[] = [];
    const recorder = new WorkoutRecorder({
      now: clock.now,
      onFinalized: (recording) => finalized.push(recording),
    });

    recorder.handleMessage(stats(true, 70));
    clock.advance(10);
    recorder.handleMessage(drive([9, 8, 7]));
    clock.advance(10);
    recorder.handleMessage(stats(true, 75));
    clock.advance(10);
    const result = recorder.handleMessage(stats(false, 40));

    assert.equal(result === null, false);
    if (!result) return;
    assert.equal(result.messages.length, 4);
    assert.equal(result.messages[0]?.message.type, 'stats');
    assert.equal(result.messages[1]?.message.type, 'drive');
    assert.equal(result.messages[2]?.message.type, 'stats');
    assert.equal(result.messages[3]?.message.type, 'stats');
    assert.deepEqual(
      result.messages.map((entry) => entry.receivedAtMs),
      [500, 510, 520, 530],
    );
    assert.equal(finalized.length, 1);
  });

  it('finalizes when running becomes false after a start', () => {
    const recorder = new WorkoutRecorder({ now: () => 1 });
    recorder.handleMessage(stats(true));
    const result = recorder.handleMessage(stats(false));
    assert.equal(result === null, false);
    if (!result) return;
    assert.equal(result.endReason, 'completed');
    assert.equal(recorder.getState(), 'idle');
  });

  it('never starts or stops from drive messages alone', () => {
    const recorder = new WorkoutRecorder({ now: () => 0 });
    assert.equal(recorder.handleMessage(drive([1])), null);
    assert.equal(recorder.getState(), 'idle');

    recorder.handleMessage(stats(true));
    assert.equal(recorder.handleMessage(drive([2])), null);
    assert.equal(recorder.getState(), 'recording');
  });

  it('ignores telemetry outside an active workout', () => {
    const recorder = new WorkoutRecorder({ now: () => 0 });
    assert.equal(recorder.handleMessage(stats(false)), null);
    assert.equal(recorder.handleMessage(drive([])), null);
    assert.equal(recorder.getState(), 'idle');
  });

  it('finalizes an incomplete workout on disconnect while recording', () => {
    const clock = createClock(100);
    const recorder = new WorkoutRecorder({ now: clock.now });
    recorder.handleMessage(stats(true));
    clock.advance(5);
    recorder.handleMessage(drive([1]));
    clock.advance(5);
    const result = recorder.handleDisconnect();

    assert.equal(result === null, false);
    if (!result) return;
    assert.equal(result.endReason, 'disconnected');
    assert.equal(result.messages.length, 2);
    assert.equal(result.startedAtMs, 100);
    assert.equal(result.endedAtMs, 110);
    assert.equal(recorder.getState(), 'idle');
  });

  it('does not create a recording on disconnect while idle', () => {
    const recorder = new WorkoutRecorder({ now: () => 0 });
    assert.equal(recorder.handleDisconnect(), null);
    assert.equal(recorder.getState(), 'idle');
  });

  it('does not finalize twice on duplicate stop or disconnect', () => {
    const finalized: WorkoutRecording[] = [];
    const recorder = new WorkoutRecorder({
      now: () => 0,
      onFinalized: (recording) => finalized.push(recording),
    });

    recorder.handleMessage(stats(true));
    const first = recorder.handleMessage(stats(false));
    const secondStop = recorder.handleMessage(stats(false));
    const secondDisconnect = recorder.handleDisconnect();

    assert.equal(first === null, false);
    assert.equal(secondStop, null);
    assert.equal(secondDisconnect, null);
    assert.equal(finalized.length, 1);

    recorder.handleMessage(stats(true));
    const disconnectOnce = recorder.handleDisconnect();
    const disconnectTwice = recorder.handleDisconnect();
    assert.equal(disconnectOnce === null, false);
    assert.equal(disconnectTwice, null);
    assert.equal(finalized.length, 2);
  });

  it('can start a second workout after the first is finalized', () => {
    const recorder = new WorkoutRecorder({ now: () => 0 });
    recorder.handleMessage(stats(true, 1));
    const first = recorder.handleMessage(stats(false, 1));
    assert.equal(first === null, false);
    if (!first) return;
    assert.equal(first.endReason, 'completed');

    recorder.handleMessage(stats(true, 2));
    recorder.handleMessage(drive([4, 5]));
    const second = recorder.handleMessage(stats(false, 2));
    assert.equal(second === null, false);
    if (!second) return;
    assert.equal(second.endReason, 'completed');
    assert.equal(second.messages.length, 3);
    assert.equal(recorder.getState(), 'idle');
  });
});
