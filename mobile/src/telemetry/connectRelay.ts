import { PermissionsAndroid, Platform } from 'react-native';
import type { BleManager, Device, Subscription } from 'react-native-ble-plx';

import { RelayStream, type ReceivedTelemetry } from './relayStream';

const SERVICE = 'be7a1000-7c2d-4e58-a608-dbea31094d61';
const PHONE_TELEMETRY = 'be7a1005-7c2d-4e58-a608-dbea31094d61';

type RelayCallbacks = {
  onStatus: (status: string) => void;
  onData: (records: ReceivedTelemetry[]) => void;
  onError: (error: string) => void;
};

async function requestBluetooth() {
  if (Platform.OS !== 'android') return;
  const { BLUETOOTH_SCAN, BLUETOOTH_CONNECT, ACCESS_FINE_LOCATION } =
    PermissionsAndroid.PERMISSIONS;
  const permissions =
    Number(Platform.Version) >= 31 ? [BLUETOOTH_SCAN, BLUETOOTH_CONNECT] : [ACCESS_FINE_LOCATION];
  const result = await PermissionsAndroid.requestMultiple(permissions);
  if (permissions.some((permission) => result[permission] !== PermissionsAndroid.RESULTS.GRANTED)) {
    throw new Error('Allow Bluetooth scanning and connection in phone settings.');
  }
}

export function connectRelay({ onStatus, onData, onError }: RelayCallbacks) {
  const stream = new RelayStream();
  const subscriptions: Subscription[] = [];
  let manager: BleManager | undefined;
  let deviceId: string | undefined;
  let closed = false;
  let closing: Promise<void> | undefined;
  let timeout: ReturnType<typeof setTimeout> | undefined;

  function close(): Promise<void> {
    if (closing) return closing;
    closed = true;
    clearTimeout(timeout);
    for (const subscription of subscriptions) subscription.remove();
    closing = (async () => {
      if (!manager) return;
      await manager.stopDeviceScan().catch(() => undefined);
      if (deviceId) await manager.cancelDeviceConnection(deviceId).catch(() => undefined);
      await manager.destroy();
    })();
    return closing;
  }

  function fail(error: unknown) {
    if (closed) return;
    onError(error instanceof Error ? error.message : String(error));
  }

  async function connect(device: Device, ble: BleManager) {
    deviceId = device.id;
    onStatus('Connecting to the Freenove…');
    await ble.stopDeviceScan();
    if (closed) return;
    const peripheral = await ble.connectToDevice(device.id);
    if (closed) return;
    subscriptions.push(ble.onDeviceDisconnected(device.id, () => fail('The Freenove disconnected.')));
    await peripheral.discoverAllServicesAndCharacteristics();
    if (closed) return;
    const characteristics = await peripheral.characteristicsForService(SERVICE);
    if (closed) return;
    if (!characteristics.some((item) => item.uuid.toLowerCase() === PHONE_TELEMETRY && item.isNotifiable)) {
      throw new Error('Phone relay unavailable. Check the Freenove firmware.');
    }
    subscriptions.push(
      peripheral.monitorCharacteristicForService(SERVICE, PHONE_TELEMETRY, (error, value) => {
        if (closed) return;
        if (error) return fail(error);
        if (!value?.value) return;
        const records = stream.push(value.value);
        if (records.length > 0) onData(records);
      }),
    );
    clearTimeout(timeout);
    onStatus('Connected. Waiting for telemetry…');
  }

  async function start() {
    await requestBluetooth();
    const { BleManager: Manager, State } = await import('react-native-ble-plx');
    if (closed) return;
    const ble = new Manager();
    manager = ble;
    let scanning = false;
    timeout = setTimeout(() => fail('Freenove connection timed out. Check that the monitor is powered on.'), 20000);
    subscriptions.push(
      ble.onStateChange((state) => {
        if (closed) return;
        if (state === State.PoweredOff) return fail('Turn on Bluetooth and try again.');
        if (state === State.Unauthorized) return fail('Allow DragonSense to use Bluetooth in Settings.');
        if (state === State.Unsupported) return fail('Bluetooth LE is unavailable on this device.');
        if (state !== State.PoweredOn || scanning) return;
        scanning = true;
        onStatus('Scanning for perg-force-display…');
        void ble.startDeviceScan([SERVICE], null, (error, device) => {
          if (closed || deviceId) return;
          if (error) return fail(error);
          if (device) void connect(device, ble).catch(fail);
        }).catch(fail);
      }, true),
    );
  }

  void start().catch(fail);
  return { close };
}
