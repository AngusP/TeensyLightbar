#include <Time.h>
#include <TimeAlarms.h>
#include <Adafruit_NeoPixel.h>


#define STRIPS 5
#define PIXELS_PER_STRIP 55

#define PIXELS STRIPS * PIXELS_PER_STRIP
#define PIN 6
#define SYSTEM_ID 0xEDED

// Parameter 1 = number of pixels in strip
// Parameter 2 = Arduino pin number (most are valid)
// Parameter 3 = pixel type flags, add together as needed:
//   NEO_KHZ800  800 KHz bitstream (most NeoPixel products w/WS2812 LEDs)
//   NEO_KHZ400  400 KHz (classic 'v1' (not v2) FLORA pixels, WS2811 drivers)
//   NEO_GRB     Pixels are wired for GRB bitstream (most NeoPixel products)
//   NEO_RGB     Pixels are wired for RGB bitstream (v1 FLORA pixels, not v2)
Adafruit_NeoPixel strip = Adafruit_NeoPixel(PIXELS, PIN, NEO_GRB + NEO_KHZ800);

boolean debug = true;

// Which channel we're listening on. 0 = Broadcast, Range 1 - 255
byte channel = 1;

// Hold the ID of the Pixel We're on.
unsigned int which = 0;

byte rcvCmd = 255;
byte rcvChan = 0;
uint16_t dataSize = 0;

// Stop listening for a frame over serial after this many milliseconds
const int timeout = 10000;
time_t lastframe = 0;

// Set the default time to 2014-01-01T00:00+0
const time_t dtime = 1388534400;

// 4 Byte header to store Channel, Command & Length
byte header[4] = {0, 255, 0, 0};

// Large buffer to hold RGB values for pixels
static byte frameBuffer[PIXELS * 3];

unsigned int datasize = 0;

/* 

Open Pixel Control packet spec

|---------|---------|-------------|-------------------------|
| Channel | Command | Length (n)  | Data                    |
|---------|---------|-------------|-------------------------|
| 0 - 255 | 0 - 255 | MSB  |  LSB | n bytes of message data |
|---------|---------|-------------|-------------------------|

Command:   
    0   : 0x00 : Set Pixel Colours
    255 : 0xFF : System Exclusive
      - The first two bytes of the Data block should be the SYSTEM_ID,
        Unique to this device. 0x0001 is reserved for the FadeCandy.

Length:
    Exact length of the Data block, in bytes, range of 0 - 65535.
    For command 0 (Set Pixel Colours) the Data block will be 3 * numPixels.
    
Data:
    Contains n bytes of data
    If the command was SysEx, the first two bytes are the system ID
*/


void setup()
{
  // Set Time to use Teensy 3.0's RTC to sync time
  setSyncProvider(getRTCTime);
  
  Serial.begin(115200);
  while(!Serial);
  Serial.setTimeout(500);
  
  strip.begin();
  strip.show();
  
  // Initialise the pixel buffer to zeros
  for(int i=0; i<sizeof(frameBuffer); i++){
    frameBuffer[i] = 0;
  }
  
  if (timeStatus() != timeSet) {
    Serial.println("ERROR: Unable to sync with the RTC");
  } else {
    if (debug) Serial.println("DEBUG: RTC has set the system time");
  }
}



void loop()
{
  // Read the 4 Header Bytes
  if (Serial.dtr() && Serial.available() >= sizeof(header)){
    for(int i=0; i<sizeof(header); i++){
      header[i] = Serial.read();
    }
    which = 0;
  }
  
  rcvChan  = header[0];
  rcvCmd   = header[1];
  dataSize = header[2] << 8 | header[3];
  
  if(debug) Serial.printf("DEBUG: Channel: %u Command: %u Data Packet size: %u bytes.\n", rcvChan, rcvCmd, dataSize);
  
  // If Broadcast or our Channel
  if (rcvChan == 0 || rcvChan == channel){
    
    // Command to write pixels
    if (rcvCmd == 0){
      fillBuffer(dataSize);
      writeFrame();
    }
    
    // System Exclusive Command
    if (rcvCmd == 255 && checkID()){
      /* 
      So we've taken the header & the first two data bytes (checkID) meaning
      the remaining bytes in the Serial buffer should be a SysEx command.
      The first byte determines which setting
      */
      switch (Serial.read()) {
        
        // Sync Time
        case 'T': {
          time_t tmp = timeSync();
          if (tmp != 0) {
            setTime(tmp);
            if (debug) Serial.printf("DEBUG: Set time to %u seconds since Epoch", tmp);
          }
          break;
        }
        
        // Change Channel
        case 'C':
          channel = Serial.parseInt();
          if (debug) Serial.printf("DEBUG: Changed channel to %d \n", channel);
          break;
        
        // Toggle debug
        case 'D':
          debug = !debug;
          Serial.printf("DEBUG: Toggled debug to: %d \n", debug);
          break;
        
        // Default to nothing
        default:
          if (debug) Serial.println("DEBUG: No valid command found in Serial buffer after Header indicated it's presence");
          break;
          
      }
    }
  }
  
}



/** /-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\ **/


void writeFrame()
{
  which = 0;
  for(int i=0; i<sizeof(frameBuffer); i+= 3){
    strip.setPixelColor(which, frameBuffer[i], frameBuffer[i+1], frameBuffer[i+2]);
    which++;
  }
  if (debug) Serial.printf("DEBUG: Wrote frame at %u \n", now());
  lastframe = now();
}


/** /-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\ **/


void fillBuffer(uint16_t packetSize)
{
  if(packetSize >= sizeof(frameBuffer)) packetSize = sizeof(frameBuffer);
  
  int i=0;
  while( Serial.available() >= packetSize ){
    frameBuffer[i] = Serial.read();
    i++; // Next byte in the buffer
    packetSize--; // Remaining bytes to read
  }
}


/** /-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\ **/


// Returns a NeoPixel Colour
uint32_t colourTemp(uint32_t temp)
{
  // Accepts Temperature, temp, in Kelvin
  // Between 1000K and 40,000K
  int32_t red;
  int32_t green;
  int32_t blue;

  temp /= 100;

  // Calculate Red:
  if(temp <= 66){
    red = 255;
  } 
  else {
    red = temp - 60;
    red = 329.698727446 * pow(red,-0.1332047592);
    if (red < 0) red = 0;
    if (red > 255) red = 255;
  }

  // Calculate Green
  if (temp <= 66){
    green = temp;
    green = 99.4708025861 * log(green) - 161.1195681661;
    if (green < 0) green = 0;
    if (green > 255) green = 255;
  } 
  else {
    green = temp - 60;
    green = 288.1221695283 * pow(green, -0.0755148492);
    if (green < 0) green = 0;
    if (green > 255) green = 255;
  }
  
  // Calculate Blue
  if (temp >= 66){
    blue = 255;
  } else {
    if (temp <= 19) {
      blue = 0;
    } else {
      blue = temp - 10;
      blue = 138.5177312231 * log(blue) - 305.0447927307;
      if (blue < 0) blue = 0;
      if (blue > 255) blue = 255;
    }
  }
  red = (uint8_t) red;
  green = (uint8_t) green;
  blue = (uint8_t) blue;
  
  return ((uint32_t)red << 16) | ((uint32_t)green <<  8) | blue; 
}


/** /-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\ **/


time_t timeSync()
{
  time_t pctime = 0L;
  if(Serial.available()) {
     pctime = Serial.parseInt();
     if(dtime > pctime){
       pctime = 0L;
     }
  }
  return pctime;
}


/** /-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\ **/


time_t getRTCTime()
{
  return Teensy3Clock.get();
}



/** /-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\ **/

// Checks if the first two bytes of the Serial buffer match the System ID
boolean checkID()
{
  if ( (Serial.read() << 8 | Serial.read()) == SYSTEM_ID){
    return true;
  } else {
    return false;
  }
}



