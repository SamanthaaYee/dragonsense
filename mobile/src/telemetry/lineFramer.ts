/**
 * Max characters in one telemetry line, excluding the newline.
 * Matches firmware `BleLineFramer` / `BasicLineFramer<112>` where capacity
 * includes the terminator, so the longest stored line is Capacity - 1.
 */
export const TELEMETRY_MAX_LINE_LENGTH = 111;

/**
 * Buffers UTF-8 text chunks from BLE notifications into newline-delimited
 * messages. Incomplete lines stay buffered across `push` calls.
 */
export class LineFramer {
  private buffer = '';
  private overflow = false;
  private readonly maxLineLength: number;

  constructor(maxLineLength: number = TELEMETRY_MAX_LINE_LENGTH) {
    if (!Number.isInteger(maxLineLength) || maxLineLength < 1) {
      throw new RangeError('maxLineLength must be an integer >= 1');
    }
    this.maxLineLength = maxLineLength;
  }

  /** Discard any partial line (call on BLE disconnect). */
  reset(): void {
    this.buffer = '';
    this.overflow = false;
  }

  /**
   * Feed the next text chunk. Returns every complete line closed by `\n`
   * in this chunk (possibly zero). Empty lines are ignored. Oversized
   * lines are dropped when their terminating newline arrives.
   */
  push(chunk: string): string[] {
    const lines: string[] = [];

    for (let i = 0; i < chunk.length; i += 1) {
      const char = chunk[i];

      if (char === '\n') {
        if (!this.overflow && this.buffer.length > 0) {
          lines.push(this.buffer);
        }
        this.reset();
        continue;
      }

      // Support CRLF: ignore CR the same way the firmware framer does.
      if (char === '\r') continue;

      // Match firmware: a NUL marks the line as overflow and it is dropped.
      if (char === '\0') {
        this.overflow = true;
        continue;
      }

      if (this.buffer.length >= this.maxLineLength) {
        this.overflow = true;
      }

      if (!this.overflow) {
        this.buffer += char;
      }
    }

    return lines;
  }
}
