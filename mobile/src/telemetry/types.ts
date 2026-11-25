/** Phone-side shapes for PERG display telemetry (`stats` / `drive` lines). */

export const DRIVE_POINT_COUNT = 32;

export interface StatsTelemetry {
  type: 'stats';
  /** Wire value `1` means the workout/session is running. */
  running: boolean;
  rpm: number;
  pace: number;
  distance: number;
  elapsed: number;
  watts: number;
  spm: number;
  /** Positive means right foot; range is -100 to 100. */
  balance: number;
}

export interface DriveTelemetry {
  type: 'drive';
  /**
   * Relative drive-curve samples (0–255), or `[]` when the logger sent an
   * empty `drive,` line (curve cleared / not ready).
   */
  points: number[];
}

export type TelemetryMessage = StatsTelemetry | DriveTelemetry;

export type ParseTelemetryResult =
  | { ok: true; message: TelemetryMessage }
  | { ok: false; error: string };
