/**
 * Narrow BLE manager surface used by PergBleSession.
 * Kept local so unit tests can inject fakes without loading the native module.
 */

export type BleSubscription = {
  remove: () => void;
};

export type BleScanDevice = {
  id: string;
  name: string | null;
  localName?: string | null;
};

export type BleCharacteristic = {
  value: string | null;
};

export type BleManagerPort = {
  startDeviceScan: (
    UUIDs: string[] | null,
    options: object | null,
    listener: (error: Error | null, device: BleScanDevice | null) => void,
  ) => Promise<void>;
  stopDeviceScan: () => Promise<void>;
  connectToDevice: (deviceIdentifier: string) => Promise<BleScanDevice>;
  cancelDeviceConnection: (deviceIdentifier: string) => Promise<unknown>;
  onDeviceDisconnected: (
    deviceIdentifier: string,
    listener: (error: Error | null, device: BleScanDevice | null) => void,
  ) => BleSubscription;
  discoverAllServicesAndCharacteristicsForDevice: (
    deviceIdentifier: string,
  ) => Promise<unknown>;
  monitorCharacteristicForDevice: (
    deviceIdentifier: string,
    serviceUUID: string,
    characteristicUUID: string,
    listener: (error: Error | null, characteristic: BleCharacteristic | null) => void,
    transactionId?: string,
  ) => BleSubscription;
  cancelTransaction?: (transactionId: string) => Promise<void>;
};
