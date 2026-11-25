import { Buffer } from 'buffer';

import { LineFramer } from './lineFramer.ts';
import { parseTelemetry } from './parseTelemetry.ts';
import type { TelemetryMessage } from './types.ts';

export interface ReceivedTelemetry {
  receivedAtMs: number;
  line: string;
  message: TelemetryMessage;
}

export class RelayStream {
  private readonly framer = new LineFramer();

  push(base64: string, receivedAtMs = Date.now()): ReceivedTelemetry[] {
    const received: ReceivedTelemetry[] = [];
    const chunk = Buffer.from(base64, 'base64').toString('utf8');
    for (const line of this.framer.push(chunk)) {
      const result = parseTelemetry(line);
      if (result.ok) received.push({ receivedAtMs, line, message: result.message });
    }
    return received;
  }
}

export function formatCapture(records: ReceivedTelemetry[]): string {
  return records.map((record) => JSON.stringify(record) + '\n').join('');
}
