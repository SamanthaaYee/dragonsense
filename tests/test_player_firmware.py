"""Exercise the actual player sketch with a native compiler and fake Arduino I/O."""

import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


@unittest.skipUnless(shutil.which("c++"), "Native C++ compiler is not installed")
class PlayerFirmwareTests(unittest.TestCase):
    def test_buffer_overflow_does_not_advance_signal_and_bad_packets_resync(self):
        sketch = (
            Path(__file__).resolve().parents[1]
            / "firmware/perg_signal_player/perg_signal_player.ino"
        )
        harness = r"""
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <string>
using namespace std;
const int HIGH=1, LOW=0, OUTPUT=1;
unsigned long micros(){return 1000;}
unsigned long millis(){return 1;}
void pinMode(int,int){}
void digitalWrite(int,int){}
int dacCalls=0, outputCode=-1;
void dacWrite(int,int code){dacCalls++;outputCode=code;}
struct SerialMock {
 deque<uint8_t> input;
 string output;
 void begin(unsigned long){}
 int available(){return input.size();}
 int read(){int v=input.front();input.pop_front();return v;}
 void println(const char *s){output+=string(s)+"\n";}
 void printf(const char *fmt,...){
  char buf[256];va_list args;va_start(args,fmt);
  vsnprintf(buf,sizeof(buf),fmt,args);va_end(args);output+=buf;
 }
} Serial;
"""
        harness += "\n#include " + json.dumps(str(sketch)) + "\n"
        harness += r"""
int main(){
 for(size_t i=0;i<RING_SIZE-1;i++) assert(ringPush(130));
 size_t before=ringTail;
 assert(!ringPush(131));
 assert(ringTail==before && dacCalls==0);
 playing=true;
 uint8_t sample[]={130,0};dispatch(0x01,sample,2);
 assert(!playing && outputCode==128 && ringTail==before);
 assert(Serial.output.find("ring_full")!=string::npos);
 dispatch(0x12,nullptr,0);
 assert(ringLevel()==0);
 // Oversized payload, followed by a valid STATUS packet.
 for(uint8_t b:{0xA5,0x5A,0x01,0x01,0x04}) Serial.input.push_back(b);
 for(int i=0;i<1025;i++) Serial.input.push_back(0);
 for(uint8_t b:{0xA5,0x5A,0x15,0x00,0x00}) Serial.input.push_back(b);
 serviceSerial();
 assert(rxState==RX_SYNC1 && ringLevel()==0);
 assert(Serial.output.find("packet_too_large")!=string::npos);
 assert(Serial.output.find("K level=0")!=string::npos);
 uint8_t once[]={0};dispatch(0x10,once,1);assert(playing);
 dispatch(0x11,nullptr,0);assert(!playing && outputCode==128);
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "player.cpp"
            executable = Path(tmp) / "player"
            source.write_text(harness)
            subprocess.run(
                ["c++", "-std=c++17", str(source), "-o", str(executable)],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
