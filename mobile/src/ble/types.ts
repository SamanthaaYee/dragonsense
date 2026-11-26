import type { TelemetryMessage } from '../telemetry/types.ts';

/** High-level phone BLE session state. */
export type BleConnectionState =
  | 'disconnected'
  | 'poweredOff'
  | 'unauthorized'
  | 'scanning'
  | 'connecting'
  | 'connected'
  | 'disconnecting'
  | 'error';

export type TelemetryMessageListener = (message: TelemetryMessage) => void;

export type BleConnectionStateListener = (state: BleConnectionState) => void;

export type BleErrorListener = (error: string) => void;

export type PergBleSessionListeners = {
  onStateChange?: BleConnectionStateListener;
  onMessage?: TelemetryMessageListener;
  onError?: BleErrorListener;
};
