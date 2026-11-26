import type { TelemetryMessage } from '../telemetry/types.ts';

/** Why a recording stopped. */
export type WorkoutEndReason = 'completed' | 'disconnected';

/** Recorder lifecycle: idle until `stats.running === true`. */
export type WorkoutRecorderState = 'idle' | 'recording';

/**
 * One captured telemetry sample.
 *
 * `receivedAtMs` comes from the recorder clock (default: `performance.now()`),
 * a monotonic millisecond timeline suitable for ordering and relative elapsed
 * time within the process. It is not wall-clock Unix time.
 */
export type CapturedTelemetry = {
  receivedAtMs: number;
  message: TelemetryMessage;
};

/** Finalized workout capture produced by WorkoutRecorder. */
export type WorkoutRecording = {
  startedAtMs: number;
  endedAtMs: number;
  endReason: WorkoutEndReason;
  messages: CapturedTelemetry[];
};

export type WorkoutRecorderOptions = {
  /**
   * Millisecond clock used for `receivedAtMs` / start / end timestamps.
   * Defaults to `() => performance.now()` for a consistent monotonic source
   * in Node and React Native.
   */
  now?: () => number;
  /** Called once each time a workout is finalized. */
  onFinalized?: (recording: WorkoutRecording) => void;
};
