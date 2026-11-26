import { LineFramer } from '../telemetry/lineFramer.ts';
import { parseTelemetry } from '../telemetry/parseTelemetry.ts';
import type { BleManagerPort, BleScanDevice, BleSubscription } from './bleManagerPort.ts';
import {
  PERG_DISPLAY_NAME,
  PERG_SERVICE_UUID,
  PHONE_TELEMETRY_UUID,
} from './constants.ts';
import type { BleConnectionState, PergBleSessionListeners } from './types.ts';
import { decodeBase64Utf8 } from './utf8.ts';

const ACTIVE_STATES: ReadonlySet<BleConnectionState> = new Set([
  'scanning',
  'connecting',
  'connected',
  'disconnecting',
]);

function deviceMatchesDisplay(device: BleScanDevice): boolean {
  return device.name === PERG_DISPLAY_NAME || device.localName === PERG_DISPLAY_NAME;
}

function errorMessage(error: unknown): string {
  if (error instanceof Error && error.message) return error.message;
  return String(error);
}

/**
 * UI-independent BLE session for the PERG force display phone-telemetry path.
 * Inject a {@link BleManagerPort} (real BleManager or test fake).
 */
export class PergBleSession {
  private state: BleConnectionState = 'disconnected';
  private generation = 0;
  private deviceId: string | null = null;
  private monitorSubscription: BleSubscription | null = null;
  private disconnectSubscription: BleSubscription | null = null;
  private monitorTransactionId: string | null = null;
  private readonly framer = new LineFramer();
  private connectInFlight: Promise<void> | null = null;
  private readonly listeners: PergBleSessionListeners;
  private readonly manager: BleManagerPort;

  constructor(listeners: PergBleSessionListeners, manager: BleManagerPort) {
    this.listeners = listeners;
    this.manager = manager;
  }

  getState(): BleConnectionState {
    return this.state;
  }

  /**
   * Scan for the PERG display, connect, discover services, and subscribe to
   * phone-telemetry notifications. Rejects if a session is already active.
   */
  async scanAndConnect(): Promise<void> {
    if (ACTIVE_STATES.has(this.state) || this.connectInFlight) {
      const message = `cannot start connection from state ${this.state}`;
      this.listeners.onError?.(message);
      throw new Error(message);
    }

    const generation = ++this.generation;
    this.framer.reset();
    this.setState('scanning');

    const run = this.runScanAndConnect(generation);
    this.connectInFlight = run;
    try {
      await run;
    } finally {
      if (this.connectInFlight === run) this.connectInFlight = null;
    }
  }

  /** Disconnect, unsubscribe, and reset framing. Safe to call repeatedly. */
  async disconnect(): Promise<void> {
    const generation = ++this.generation;
    this.framer.reset();

    if (this.state === 'disconnected') {
      await this.clearSubscriptions();
      this.deviceId = null;
      return;
    }

    this.setState('disconnecting');
    const deviceId = this.deviceId;
    await this.clearSubscriptions();
    this.deviceId = null;

    if (deviceId) {
      try {
        await this.manager.stopDeviceScan();
      } catch {
        // Scan may already be stopped.
      }
      try {
        await this.manager.cancelDeviceConnection(deviceId);
      } catch {
        // Already disconnected is fine.
      }
    } else {
      try {
        await this.manager.stopDeviceScan();
      } catch {
        // Ignore.
      }
    }

    if (generation === this.generation) {
      this.setState('disconnected');
    }
  }

  private async runScanAndConnect(generation: number): Promise<void> {
    let claimedDevice = false;
    let settled = false;

    await new Promise<void>((resolve, reject) => {
      const fail = (error: unknown) => {
        if (settled || generation !== this.generation) return;
        settled = true;
        void this.failSession(generation, errorMessage(error)).finally(() => {
          reject(error instanceof Error ? error : new Error(errorMessage(error)));
        });
      };

      void this.manager
        .startDeviceScan([PERG_SERVICE_UUID], null, (error, device) => {
          if (generation !== this.generation || claimedDevice || settled) return;

          if (error) {
            fail(error);
            return;
          }
          if (!device || !deviceMatchesDisplay(device)) return;

          claimedDevice = true;
          void this.connectToScannedDevice(generation, device)
            .then(() => {
              if (settled || generation !== this.generation) return;
              settled = true;
              resolve();
            })
            .catch((connectError: unknown) => {
              if (settled || generation !== this.generation) return;
              settled = true;
              void this.failSession(generation, errorMessage(connectError)).finally(() => {
                reject(
                  connectError instanceof Error
                    ? connectError
                    : new Error(errorMessage(connectError)),
                );
              });
            });
        })
        .catch((startError: unknown) => {
          fail(startError);
        });
    });
  }

  private async connectToScannedDevice(
    generation: number,
    device: BleScanDevice,
  ): Promise<void> {
    if (generation !== this.generation) return;

    await this.manager.stopDeviceScan();
    if (generation !== this.generation) return;

    this.setState('connecting');
    const connected = await this.manager.connectToDevice(device.id);
    if (generation !== this.generation) {
      try {
        await this.manager.cancelDeviceConnection(connected.id);
      } catch {
        // Superseded session.
      }
      return;
    }

    this.deviceId = connected.id;
    this.disconnectSubscription = this.manager.onDeviceDisconnected(
      connected.id,
      (error) => {
        if (generation !== this.generation) return;
        this.handleUnexpectedDisconnect(generation, error);
      },
    );

    await this.manager.discoverAllServicesAndCharacteristicsForDevice(connected.id);
    if (generation !== this.generation) return;

    this.monitorTransactionId = `perg-phone-telemetry-${generation}`;
    this.monitorSubscription = this.manager.monitorCharacteristicForDevice(
      connected.id,
      PERG_SERVICE_UUID,
      PHONE_TELEMETRY_UUID,
      (error, characteristic) => {
        if (generation !== this.generation) return;
        this.handleNotification(generation, error, characteristic?.value ?? null);
      },
      this.monitorTransactionId,
    );

    if (generation !== this.generation) return;
    this.setState('connected');
  }

  private handleNotification(
    generation: number,
    error: Error | null,
    value: string | null,
  ): void {
    if (generation !== this.generation) return;

    if (error) {
      const message = errorMessage(error);
      if (/cancel/i.test(message)) return;
      this.listeners.onError?.(message);
      return;
    }

    if (value == null) return;

    const decoded = decodeBase64Utf8(value);
    if (!decoded.ok) {
      this.listeners.onError?.(decoded.error);
      return;
    }

    let lines: string[];
    try {
      lines = this.framer.push(decoded.text);
    } catch (frameError) {
      this.listeners.onError?.(errorMessage(frameError));
      return;
    }

    for (const line of lines) {
      if (generation !== this.generation) return;
      const parsed = parseTelemetry(line);
      if (parsed.ok) {
        this.listeners.onMessage?.(parsed.message);
      } else {
        this.listeners.onError?.(parsed.error);
      }
    }
  }

  private handleUnexpectedDisconnect(
    generation: number,
    error: Error | null,
  ): void {
    if (generation !== this.generation) return;

    // Invalidate in-flight notification callbacks from this connection.
    this.generation += 1;
    this.framer.reset();
    this.deviceId = null;
    this.setState('disconnected');
    this.listeners.onError?.(
      error ? errorMessage(error) : 'PERG display disconnected unexpectedly',
    );
    void this.clearSubscriptions();
  }

  private async failSession(generation: number, message: string): Promise<void> {
    if (generation !== this.generation) return;
    this.listeners.onError?.(message);
    this.framer.reset();
    await this.clearSubscriptions();
    const deviceId = this.deviceId;
    this.deviceId = null;
    try {
      await this.manager.stopDeviceScan();
    } catch {
      // Ignore.
    }
    if (deviceId) {
      try {
        await this.manager.cancelDeviceConnection(deviceId);
      } catch {
        // Ignore.
      }
    }
    if (generation === this.generation) {
      this.setState('error');
      this.setState('disconnected');
    }
  }

  private async clearSubscriptions(): Promise<void> {
    const monitor = this.monitorSubscription;
    const disconnect = this.disconnectSubscription;
    const transactionId = this.monitorTransactionId;
    this.monitorSubscription = null;
    this.disconnectSubscription = null;
    this.monitorTransactionId = null;

    monitor?.remove();
    disconnect?.remove();

    if (transactionId && this.manager.cancelTransaction) {
      try {
        await this.manager.cancelTransaction(transactionId);
      } catch {
        // Ignore.
      }
    }
  }

  private setState(state: BleConnectionState): void {
    if (this.state === state) return;
    this.state = state;
    this.listeners.onStateChange?.(state);
  }
}
