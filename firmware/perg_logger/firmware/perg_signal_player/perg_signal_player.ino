/*
 * Perg Signal Player (ESP32)
 *
 * Plays a recorded Hall-sensor waveform (e.g. session/perg_data.csv) out of
 * the ESP32's built-in DAC so the Perg monitor's SPD jack receives the same
 * signal the ESP32 recorded. The Python tool replay_lin_probe.py streams the
 * samples over USB serial and simultaneously sniffs the monitor's LIN port
 * (see perg_lin_sniffer.ino).
 *
 * Output pin:
 *   GPIO 25 = DAC1, 8-bit, 0..~3.3 V, AC-coupled into the SPD jack
 *   through a series 10 µF cap + 470 Ω resistor with a 100 kΩ bleed
 *   (see docs/replay_wiring.md). The DAC idles at mid-scale (128),
 *   which the coupling turns into 0 V on the jack; replayed values
 *   swing the jack bipolar, roughly -1.65 .. +1.16 V for this recording.
 *
 * Sample timing:
 *   The Python host resamples recordings to 1 kHz. Playback consumes one entry
 *   per 1 ms tick, scheduled off micros() like perg_logger.ino samples its
 *   ADC. No hardware timer is used, so the sketch compiles on both the old
 *   and the new (3.x) arduino-esp32 timer APIs.
 *
 * Ring entry encoding (16-bit):
 *   0x0000..0x00FF : DAC output = low byte for this tick
 *   0x8000..0xFFFF : hold the current DAC value for (low 15 bits) ticks
 *                    (the host inserts these across gaps in the recording)
 *
 * Serial protocol (115200 baud, binary packets):
 *   A5 5A <cmd> <len_lo> <len_hi> <payload...>
 *   cmd 0x01 PLAY: payload = ring entries (uint16 little-endian)
 *   cmd 0x10 START: payload[0] = loop flag (1 = loop the stream)
 *   cmd 0x11 STOP
 *   cmd 0x12 FLUSH (clear ring)
 *   cmd 0x14 SET_DAC: payload[0] = DAC value (manual mode, stops playback)
 *   cmd 0x15 STATUS: replies with an ASCII "K ..." status line
 *   Unknown/garbled bytes are skipped until the magic re-syncs.
 */

const int DAC_PIN = 25;          // DAC1
const int LED_PIN = 2;           // on-board LED: heartbeat while playing
const unsigned long BAUD = 115200;

const unsigned long TICK_US = 1000;    // 1 sample per 1 ms, matches recording
const size_t RING_SIZE = 16384;        // power of two; 16k entries = ~16 s
const size_t RING_MASK = RING_SIZE - 1;

uint16_t ring[RING_SIZE];
volatile size_t ringHead = 0;    // write index (producer: this sketch's loop)
volatile size_t ringTail = 0;    // read index (consumer: tick scheduler)

bool playing = false;
bool loopMode = false;
unsigned long nextTickUs = 0;
uint8_t lastDac = 128;           // idle = mid-scale = 0 V on the AC-coupled jack
uint16_t holdRemaining = 0;      // ticks left in the current hold entry

// ---- packet parser state ----
enum RxState { RX_SYNC1, RX_SYNC2, RX_CMD, RX_LEN1, RX_LEN2, RX_PAYLOAD, RX_DISCARD };
RxState rxState = RX_SYNC1;
uint8_t rxCmd = 0;
uint16_t rxLen = 0;
uint16_t rxPos = 0;
uint8_t rxPayload[1024];

unsigned long lastLedMs = 0;
bool ledOn = false;

bool ringPush(uint16_t value) {
  size_t next = (ringHead + 1) & RING_MASK;
  if (next == ringTail) return false; // never consume DAC samples off the clock
  ring[ringHead] = value;
  ringHead = next;
  return true;
}

size_t ringLevel() {
  return (ringHead - ringTail) & RING_MASK;
}

// Consume one ring entry and honour the 1 ms tick timeline. Called whenever
// at least one tick has passed since the last call.
void consumeTick() {
  if (holdRemaining > 0) {
    holdRemaining--;
    return;
  }
  if (ringTail == ringHead) {
    return; // starved: keep outputting lastDac until data arrives
  }
  uint16_t entry = ring[ringTail];
  ringTail = (ringTail + 1) & RING_MASK;

  if (entry & 0x8000) {
    holdRemaining = (entry & 0x7FFF) - 1; // hold includes this tick
    return;
  }
  lastDac = entry & 0x00FF;
  dacWrite(DAC_PIN, lastDac);
}

// Handle one complete packet.
void dispatch(uint8_t cmd, uint8_t *payload, uint16_t len) {
  switch (cmd) {
    case 0x01: // PLAY: 16-bit little-endian ring entries
      for (uint16_t i = 0; i + 1 < len; i += 2) {
        uint16_t entry = payload[i] | (payload[i + 1] << 8);
        if (!ringPush(entry)) {
          playing = false;
          dacWrite(DAC_PIN, 128);
          Serial.println("K error ring_full; playback stopped");
          return;
        }
      }
      break;

    case 0x10: // START
      playing = true;
      loopMode = len > 0 && payload[0] == 1;
      holdRemaining = 0;
      nextTickUs = micros() + TICK_US;
      Serial.println("K playing");
      break;

    case 0x11: // STOP: park the DAC mid-scale (128 = 0 V on the jack;
               // parking at 0 would hold ~-1.65 V on the tip until the
               // bleed resistor discharges the coupling cap)
      playing = false;
      holdRemaining = 0;
      dacWrite(DAC_PIN, 128);
      Serial.println("K stopped");
      break;

    case 0x12: // FLUSH
      ringTail = ringHead; // empty
      holdRemaining = 0;
      Serial.println("K flushed");
      break;

    case 0x14: // SET_DAC (manual probing, stops playback)
               // the jack sees (value - 128) / 255 * 3.3 V; 128 = 0 V
      playing = false;
      holdRemaining = 0;
      lastDac = len > 0 ? payload[0] : 128;
      dacWrite(DAC_PIN, lastDac);
      Serial.println("K dac set");
      break;

    case 0x15: // STATUS
      Serial.printf("K level=%u playing=%d loop=%d dac=%d free=%u\n",
                    (unsigned)ringLevel(), (int)playing, (int)loopMode,
                    (int)lastDac,
                    (unsigned)(RING_SIZE - ringLevel() - 1));
      break;

    default:
      Serial.println("K err cmd");
      break;
  }
}

void serviceSerial() {
  while (Serial.available() > 0) {
    uint8_t b = (uint8_t)Serial.read();
    switch (rxState) {
      case RX_SYNC1:
        if (b == 0xA5) rxState = RX_SYNC2;
        break;
      case RX_SYNC2:
        rxState = (b == 0x5A) ? RX_CMD : RX_SYNC1;
        break;
      case RX_CMD:
        rxCmd = b;
        rxState = RX_LEN1;
        break;
      case RX_LEN1:
        rxLen = b;
        rxState = RX_LEN2;
        break;
      case RX_LEN2:
        rxLen |= (uint16_t)b << 8;
        rxPos = 0;
        if (rxLen == 0) {
          dispatch(rxCmd, rxPayload, 0);
          rxState = RX_SYNC1;
        } else if (rxLen <= sizeof(rxPayload)) {
          rxState = RX_PAYLOAD;
        } else {
          rxState = RX_DISCARD; // malformed lengths cannot overwrite memory
        }
        break;
      case RX_DISCARD:
        if (++rxPos >= rxLen) {
          rxState = RX_SYNC1;
          Serial.println("K error packet_too_large");
        }
        break;
      case RX_PAYLOAD:
        rxPayload[rxPos++] = b;
        if (rxPos >= rxLen) {
          dispatch(rxCmd, rxPayload, rxLen);
          rxState = RX_SYNC1;
        }
        break;
    }
  }
}

void setup() {
  Serial.begin(BAUD);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  // DAC idles at mid-scale (128) so the AC-coupled jack sits at 0 V.
  dacWrite(DAC_PIN, lastDac);
  Serial.println("K perg_signal_player ready");
}

void loop() {
  serviceSerial();

  // ---- playback scheduler (same timeline trick as perg_logger.ino) ----
  if (playing) {
    unsigned long now = micros();
    while ((long)(now - nextTickUs) >= 0) {
      nextTickUs += TICK_US;
      // If we fell far behind (e.g. ring push blocked), skip ahead instead of
      // playing a burst of back-to-back catch-up ticks.
      if ((long)(now - nextTickUs) >= 0) {
        nextTickUs = now + TICK_US;
      }
      consumeTick();
    }
  }

  // LED: slow blink while playing, off when idle, fast when ring is filling.
  unsigned long now = millis();
  unsigned long period = ringLevel() > (RING_SIZE * 3) / 4 ? 100 : 500;
  if (now - lastLedMs >= period) {
    lastLedMs = now;
    ledOn = !ledOn;
    digitalWrite(LED_PIN, playing && ledOn ? HIGH : LOW);
  }
}
