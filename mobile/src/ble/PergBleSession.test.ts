import assert from 'node:assert/strict';
import { describe, it } from 'node:test';

import type {
  BleCharacteristic,
  BleManagerPort,
  BleScanDevice,
  BleSubscription,
} from './bleManagerPort.ts';
import { PERG_DISPLAY_NAME, PERG_SERVICE_UUID, PHONE_TELEMETRY_UUID } from './constants.ts';
import { PergBleSession } from './PergBleSession.ts';
import type { BleConnectionState } from './types.ts';
import type { TelemetryMessage } from '../telemetry/types.ts';

function encodeBase64(text: string): string {
  const bytes = new TextEncoder().encode(text);
  let binary = '';
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary);
}

class FakeBleManager implements BleManagerPort {
  scanListener:
    | ((error: Error | null, device: BleScanDevice | null) => void)
    | null = null;
  disconnectListener:
    | ((error: Error | null, device: BleScanDevice | null) => void)
    | null = null;
  monitorListener:
    | ((error: Error | null, characteristic: BleCharacteristic | null) => void)
    | null = null;
  scanUuids: string[] | null = null;
  connectedId: string | null = null;
  cancelledIds: string[] = [];
  stopScanCount = 0;
  discoverCount = 0;
  monitorCalls = 0;
  cancelledTransactions: string[] = [];

  async startDeviceScan(
    UUIDs: string[] | null,
    _options: object | null,
    listener: (error: Error | null, device: BleScanDevice | null) => void,
  ): Promise<void> {
    this.scanUuids = UUIDs;
    this.scanListener = listener;
  }

  async stopDeviceScan(): Promise<void> {
    this.stopScanCount += 1;
    this.scanListener = null;
  }

  async connectToDevice(deviceIdentifier: string): Promise<BleScanDevice> {
    this.connectedId = deviceIdentifier;
    return { id: deviceIdentifier, name: PERG_DISPLAY_NAME, localName: PERG_DISPLAY_NAME };
  }

  async cancelDeviceConnection(deviceIdentifier: string): Promise<unknown> {
    this.cancelledIds.push(deviceIdentifier);
    this.connectedId = null;
    return { id: deviceIdentifier, name: PERG_DISPLAY_NAME };
  }

  onDeviceDisconnected(
    _deviceIdentifier: string,
    listener: (error: Error | null, device: BleScanDevice | null) => void,
  ): BleSubscription {
    this.disconnectListener = listener;
    return {
      remove: () => {
        if (this.disconnectListener === listener) this.disconnectListener = null;
      },
    };
  }

  async discoverAllServicesAndCharacteristicsForDevice(
    _deviceIdentifier: string,
  ): Promise<unknown> {
    this.discoverCount += 1;
    return {};
  }

  monitorCharacteristicForDevice(
    _deviceIdentifier: string,
    _serviceUUID: string,
    _characteristicUUID: string,
    listener: (error: Error | null, characteristic: BleCharacteristic | null) => void,
    transactionId?: string,
  ): BleSubscription {
    this.monitorCalls += 1;
    this.monitorListener = listener;
    return {
      remove: () => {
        if (this.monitorListener === listener) this.monitorListener = null;
      },
    };
  }

  async cancelTransaction(transactionId: string): Promise<void> {
    this.cancelledTransactions.push(transactionId);
  }

  emitDevice(device: BleScanDevice = {
    id: 'display-1',
    name: PERG_DISPLAY_NAME,
    localName: PERG_DISPLAY_NAME,
  }): void {
    this.scanListener?.(null, device);
  }

  emitNotification(text: string): void {
    this.monitorListener?.(null, { value: encodeBase64(text) });
  }

  emitRawBase64(value: string): void {
    this.monitorListener?.(null, { value });
  }

  emitDisconnect(error: Error | null = null): void {
    this.disconnectListener?.(error, {
      id: 'display-1',
      name: PERG_DISPLAY_NAME,
    });
  }
}

async function connectSession(manager: FakeBleManager, session: PergBleSession): Promise<void> {
  const pending = session.scanAndConnect();
  await Promise.resolve();
  manager.emitDevice();
  await pending;
}

describe('PergBleSession', () => {
  it('scans, connects, discovers, and subscribes', async () => {
    const manager = new FakeBleManager();
    const states: BleConnectionState[] = [];
    const session = new PergBleSession(
      { onStateChange: (state) => states.push(state) },
      manager,
    );

    await connectSession(manager, session);

    assert.deepEqual(manager.scanUuids, [PERG_SERVICE_UUID]);
    assert.equal(manager.stopScanCount >= 1, true);
    assert.equal(manager.discoverCount, 1);
    assert.equal(manager.monitorCalls, 1);
    assert.equal(session.getState(), 'connected');
    assert.ok(states.includes('scanning'));
    assert.ok(states.includes('connecting'));
    assert.ok(states.includes('connected'));
  });

  it('parses stats and drive notifications', async () => {
    const manager = new FakeBleManager();
    const messages: TelemetryMessage[] = [];
    const session = new PergBleSession(
      { onMessage: (message) => messages.push(message) },
      manager,
    );

    await connectSession(manager, session);
    manager.emitNotification('stats,1,85.5,120.0,250.0,45.2,120,28.0,12.5\n');
    manager.emitNotification(
      'drive,000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\n',
    );

    assert.equal(messages.length, 2);
    assert.equal(messages[0]?.type, 'stats');
    assert.equal(messages[1]?.type, 'drive');
    if (messages[1]?.type === 'drive') {
      assert.equal(messages[1].points.length, 32);
    }
  });

  it('assembles a message split across notification chunks', async () => {
    const manager = new FakeBleManager();
    const messages: TelemetryMessage[] = [];
    const session = new PergBleSession(
      { onMessage: (message) => messages.push(message) },
      manager,
    );

    await connectSession(manager, session);
    manager.emitNotification('stats,1,10,20,30,40,50,');
    manager.emitNotification('60,0.0\n');

    assert.equal(messages.length, 1);
    assert.equal(messages[0]?.type, 'stats');
    if (messages[0]?.type === 'stats') {
      assert.equal(messages[0].spm, 60);
      assert.equal(messages[0].balance, 0);
    }
  });

  it('parses multiple messages from one notification', async () => {
    const manager = new FakeBleManager();
    const messages: TelemetryMessage[] = [];
    const session = new PergBleSession(
      { onMessage: (message) => messages.push(message) },
      manager,
    );

    await connectSession(manager, session);
    manager.emitNotification('stats,0,0,0,0,0,0,0,0\ndrive,\n');

    assert.equal(messages.length, 2);
    assert.equal(messages[0]?.type, 'stats');
    assert.equal(messages[1]?.type, 'drive');
  });

  it('reports invalid base64 and malformed telemetry without crashing', async () => {
    const manager = new FakeBleManager();
    const errors: string[] = [];
    const messages: TelemetryMessage[] = [];
    const session = new PergBleSession(
      {
        onError: (error) => errors.push(error),
        onMessage: (message) => messages.push(message),
      },
      manager,
    );

    await connectSession(manager, session);
    manager.emitRawBase64('!!!!');
    manager.emitNotification('not-a-telemetry-line\n');
    manager.emitNotification('stats,1,0,0,0,0,0,0,0\n');

    assert.ok(errors.length >= 2);
    assert.equal(messages.length, 1);
    assert.equal(session.getState(), 'connected');
  });

  it('handles unexpected disconnect', async () => {
    const manager = new FakeBleManager();
    const states: BleConnectionState[] = [];
    const errors: string[] = [];
    const session = new PergBleSession(
      {
        onStateChange: (state) => states.push(state),
        onError: (error) => errors.push(error),
      },
      manager,
    );

    await connectSession(manager, session);
    manager.emitDisconnect();

    assert.equal(session.getState(), 'disconnected');
    assert.ok(errors.some((error) => /disconnect/i.test(error)));
    assert.equal(manager.monitorListener, null);
  });

  it('disconnects and cleans up, and allows repeated disconnect', async () => {
    const manager = new FakeBleManager();
    const session = new PergBleSession({}, manager);

    await connectSession(manager, session);
    await session.disconnect();
    assert.equal(session.getState(), 'disconnected');
    assert.deepEqual(manager.cancelledIds, ['display-1']);
    assert.equal(manager.monitorListener, null);

    await session.disconnect();
    assert.equal(session.getState(), 'disconnected');
  });

  it('prevents duplicate connection attempts', async () => {
    const manager = new FakeBleManager();
    const errors: string[] = [];
    const session = new PergBleSession(
      { onError: (error) => errors.push(error) },
      manager,
    );

    const first = session.scanAndConnect();
    await Promise.resolve();
    let duplicateFailed = false;
    try {
      await session.scanAndConnect();
    } catch {
      duplicateFailed = true;
    }
    assert.equal(duplicateFailed, true);
    assert.ok(errors.some((error) => /cannot start connection/i.test(error)));

    manager.emitDevice();
    await first;
    assert.equal(session.getState(), 'connected');
    assert.equal(manager.monitorCalls, 1);
  });

  it('ignores notifications after disconnect', async () => {
    const manager = new FakeBleManager();
    const messages: TelemetryMessage[] = [];
    const session = new PergBleSession(
      { onMessage: (message) => messages.push(message) },
      manager,
    );

    await connectSession(manager, session);
    const staleMonitor = manager.monitorListener;
    await session.disconnect();
    staleMonitor?.(null, {
      value: encodeBase64('stats,1,0,0,0,0,0,0,0\n'),
    });

    assert.equal(messages.length, 0);
  });

  it('monitors the phone telemetry characteristic UUID', async () => {
    const manager = new FakeBleManager();
    let monitoredCharacteristic: string | null = null;
    const original = manager.monitorCharacteristicForDevice.bind(manager);
    manager.monitorCharacteristicForDevice = (
      deviceIdentifier,
      serviceUUID,
      characteristicUUID,
      listener,
      transactionId,
    ) => {
      monitoredCharacteristic = characteristicUUID;
      assert.equal(serviceUUID, PERG_SERVICE_UUID);
      return original(deviceIdentifier, serviceUUID, characteristicUUID, listener, transactionId);
    };

    const session = new PergBleSession({}, manager);
    await connectSession(manager, session);
    assert.equal(monitoredCharacteristic, PHONE_TELEMETRY_UUID);
  });
});
