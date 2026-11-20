// Code used to calibrate each of the sensors

float cf = 19.5; // calibration factor

int ffs1 = A0; // FlexiForce sensor is connected analog pin A0 of arduino or mega.

int ffsdata1 = 0;

float vout1;

void setup()
{
    Serial.begin(9600);
    pinMode(ffs1, INPUT);

}

void loop()
{
    ffsdata1 = analogRead(ffs1);

    vout1 = (ffsdata1 * 5.0) / 1023.0;
    vout1 = vout1 * cf;

    Serial.print("Flexi Force sensor 1: ");
    Serial.print(vout1, 3);

    Serial.println("");

    delay(100);
}