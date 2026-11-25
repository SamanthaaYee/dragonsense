/** USB recorder row from `perg_telemetry.csv` (not BLE wire format). */

export interface TelemetryCsvRow {
  time_us: number;
  rpm: number;
  pace_s: number;
  distance_m: number;
  hall_v: number;
  period_ms: number;
  elapsed_s: number;
  watts: number;
  stroke_rate_spm: number;
}

export type ParseTelemetryCsvResult =
  | { ok: true; row: TelemetryCsvRow }
  | { ok: false; error: string };
