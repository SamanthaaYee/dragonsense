# Replay interface: code assumptions, not a verified breadboard layout

The signal player runs on an ESP32 with its DAC1 on GPIO 25. It is separate
from the sensor logger and CYD display roles. Uploading it to the logger board
replaces that firmware.

The existing firmware describes this passive interface:

```text
GPIO 25 DAC -- series coupling capacitor / resistor -- monitor SPD signal
ESP32 GND ------------------------------------------- monitor signal ground
                                               |
                            output-side bleed to ground
```

The documented values are a 10 uF series capacitor, 470 ohm series resistor,
and 100 kohm output-side bleed. This records the existing code's assumptions;
actual wiring, capacitor type/polarity, connector contacts, monitor input
impedance, and acceptable voltage have not been verified. Do not infer them
from the schematic text. The sensor ADC input circuit is not the DAC output circuit.

The DAC parks at code 128 and is nominally around 1.65 V before coupling.
After the coupling network settles, the intended output is AC about monitor
signal ground. The Python conversion uses bias 1870 ADC counts and gain
0.0679, based on approximate ADC/DAC voltage scales. It is not a calibrated
voltage source. There is no need for the monitor's LIN jack for SPD replay alone.

## Proposed op-amp replacement: not implemented

The available LM741 cannot be treated as a USB/5 V rail-to-rail buffer. The
supplied TI datasheet specifies a recommended minimum of +/-10 V. A separate
+/-12 V supply or a different amplifier would be needed for the discussed design.
See the [TI LM741 datasheet](https://www.ti.com/lit/ds/symlink/lm741.pdf).

The [Microchip MCP6002](https://www.microchip.com/en-us/product/MCP6002) was
suggested as an 8-pin DIP option for 5 V operation. It has a different pinout.
Neither an op-amp circuit nor a breadboard layout has been selected or tested.
Buffering might improve loading but does not correct 1 ms playback quantization.
