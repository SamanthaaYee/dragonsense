/** Phone-side shapes for PERG display telemetry. */

export const DRIVE_POINT_COUNT = 32;

export interface FootLoads {
  leftBottom: number;
  leftTop: number;
  rightTop: number;
  rightBottom: number;
}

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
  /** Pounds-force. Older logger messages omit the four individual loads. */
  footLoads?: FootLoads;
}

export interface DriveTelemetry {
  type: 'drive';
  /**
   * Relative drive-curve samples (0–255), or `[]` when the logger sent an
   * empty `drive,` line (curve cleared / not ready).
   */
  points: number[];
}

export interface FootPeakTelemetry {
  type: 'foot_peak';
  /** Simultaneous left and right readings, in pounds-force, or null before a completed stroke. */
  loads: { left: number; right: number } | null;
}

export type TelemetryMessage = StatsTelemetry | DriveTelemetry | FootPeakTelemetry;

export type ParseTelemetryResult =
  | { ok: true; message: TelemetryMessage }
  | { ok: false; error: string };
