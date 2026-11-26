/** GATT identifiers for the Freenove PERG force display (phone is BLE central). */

export const PERG_DISPLAY_NAME = 'perg-force-display';

export const PERG_SERVICE_UUID = 'be7a1000-7c2d-4e58-a608-dbea31094d61';

/** Display → phone notify characteristic for `stats` / `drive` lines. */
export const PHONE_TELEMETRY_UUID = 'be7a1005-7c2d-4e58-a608-dbea31094d61';

/** Phone ↔ display workout plan characteristic (read/write). */
export const WORKOUT_UUID = 'be7a1004-7c2d-4e58-a608-dbea31094d61';
