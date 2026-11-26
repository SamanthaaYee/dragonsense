import type { BleManagerPort } from './bleManagerPort.ts';
import { PergBleSession } from './PergBleSession.ts';
import type { PergBleSessionListeners } from './types.ts';

/**
 * Construct a session backed by the installed BLE native module.
 * Uses a dynamic import so Node unit tests can load session code without
 * initializing the native ble-plx package.
 */
export async function createPergBleSession(
  listeners: PergBleSessionListeners,
): Promise<PergBleSession> {
  const blePlx = await import('@sfourdrinier/react-native-ble-plx');
  const manager = new blePlx.BleManager() as unknown as BleManagerPort;
  return new PergBleSession(listeners, manager);
}
