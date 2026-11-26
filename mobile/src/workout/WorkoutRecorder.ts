import type { TelemetryMessage } from '../telemetry/types.ts';
import type {
  CapturedTelemetry,
  WorkoutEndReason,
  WorkoutRecorderOptions,
  WorkoutRecorderState,
  WorkoutRecording,
} from './types.ts';

/**
 * Automatic workout capture driven only by parsed telemetry.
 *
 * - Connection alone does nothing (callers feed messages explicitly).
 * - `stats.running === true` starts a recording (that stats sample is kept).
 * - Later `stats` / `drive` samples are appended in arrival order.
 * - `stats.running === false` finalizes a normal completion.
 * - `handleDisconnect()` finalizes an incomplete workout if recording.
 * - `drive` never starts or stops a recording.
 *
 * Timestamps use an injectable millisecond clock (default `performance.now()`).
 */
export class WorkoutRecorder {
  private state: WorkoutRecorderState = 'idle';
  private messages: CapturedTelemetry[] = [];
  private startedAtMs: number | null = null;
  private readonly now: () => number;
  private readonly onFinalized?: (recording: WorkoutRecording) => void;

  constructor(options: WorkoutRecorderOptions = {}) {
    this.now = options.now ?? (() => performance.now());
    this.onFinalized = options.onFinalized;
  }

  getState(): WorkoutRecorderState {
    return this.state;
  }

  /**
   * Ingest one parsed telemetry message.
   * Returns a finalized recording when this message ends a workout; otherwise null.
   */
  handleMessage(message: TelemetryMessage): WorkoutRecording | null {
    if (message.type === 'drive') {
      if (this.state !== 'recording') return null;
      this.append(message);
      return null;
    }

    // stats
    if (this.state === 'idle') {
      if (!message.running) return null;
      this.beginRecording(message);
      return null;
    }

    // recording
    this.append(message);
    if (!message.running) {
      return this.finalize('completed');
    }
    return null;
  }

  /**
   * Notify the recorder that the BLE link dropped.
   * If a workout is in progress, finalize it as incomplete.
   */
  handleDisconnect(): WorkoutRecording | null {
    if (this.state !== 'recording') return null;
    return this.finalize('disconnected');
  }

  private beginRecording(message: TelemetryMessage): void {
    const receivedAtMs = this.now();
    this.state = 'recording';
    this.startedAtMs = receivedAtMs;
    this.messages = [{ receivedAtMs, message }];
  }

  private append(message: TelemetryMessage): void {
    this.messages.push({
      receivedAtMs: this.now(),
      message,
    });
  }

  private finalize(endReason: WorkoutEndReason): WorkoutRecording {
    const endedAtMs = this.now();
    const recording: WorkoutRecording = {
      startedAtMs: this.startedAtMs ?? endedAtMs,
      endedAtMs,
      endReason,
      messages: this.messages,
    };

    this.state = 'idle';
    this.messages = [];
    this.startedAtMs = null;
    this.onFinalized?.(recording);
    return recording;
  }
}
