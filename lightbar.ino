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

// Suggest true for development, false for production
boolean debug = true;
String lastdbg = "";

// Did the header declaration match the buffer size?
boolean match = true;

// Preprogrammed sequence selector
int mode = 0;
/** Modes:
 * 0: Listen for input
 * 1: Cycle Rainbow
 * 2: Colour Temp constant (takes temperature as argument (default below))
 **/
uint32_t temperature = 4000;

// Which channel we're listening on. 0 = Broadcast, Range 1 - 255
byte channel = 1;

// Hold the ID of the Pixel We're on.
unsigned int which = 0;

byte rcvCmd = 255;
byte rcvChan = 0;
uint16_t dataSize = 0;

time_t lastframe = 0;

// Set the default time to 1970-01-01 00:00 UTC
const time_t dtime = 0;

// 4 Byte header to store Channel, Command & Length
byte header[4] = {0, 255, 0, 0};

// Large buffer to hold RGB values for pixels
static byte frameBuffer[PIXELS * 3];

// Lock byte to remember which routine last wrote pixels
static byte lock = 0x00;
/**
 * 0x00   :   Unlocked
 * 0x01   :   Locked by Mode 2 (Colour Temp)
 **/


unsigned int datasize = 0;

/* Note that this should match void help()

   Open Pixel Control packet spec

   UNIQUE CASE: 'help' will print a help text. (0x68 0x65 0x6C 0x70)

   |-------------|-------------|-------------|--------------------------------|
   | Channel     | Command     | Length (n)  | Data                           |
   |-------------|-------------|-------------|--------------------------------|
   | 0x00 - 0xFF | 0x00 - 0xFE | MSB  |  LSB | n bytes of message data        |
   |-------------|-------------|------|------|------------|------------|------|
   | 0x00 - 0xFF | 0xFF        | MSB  |  LSB | SysID MSB  |  SysID LSB | Data |
   |-------------|-------------|------|------|------------|------------|------|

   Command:   
   0   : 0x00 : Set Pixel Colours
   255 : 0xFF : System Exclusive
   - The first two bytes of the Data block in the case of the system exclusive
     command should be the SYSTEM_ID, unique to this device. This system has ID
     0xEDED. The ID 0x0001 is reserved for the FadeCandy.

   Length:
   Exact length of the Data block, in bytes, range of 0 - 65535.
   For command 0 (Set Pixel Colours) the Data block will be 3 * numPixels.
    
   Data:
   Contains n bytes of data
   If the command was SysEx, the first two bytes are the system ID


   SYSTEM EXCLUSIVES:
   0x01  :  Set time. Expects UNIX epoch time in bytes.
   0x02  :  Get time. Returns current time as a human readable string.
   0x03  :  Set channel. Expects channel to listen on as a single byte.
   0x04  :  Get channel. Returns current channel (int) as human readable string.
   0x05  :  Toggle debug on or off. Informs with Human readable string.
   0x06  :  Set the mode. 0=Listen, 1=Light, 2=Spectrum
   0x07  :  Get mode. Prints mode (int) to serial
*/


void setup()
{
    // Set Time to use Teensy 3.0's RTC to sync time
    setSyncProvider(getRTCTime);
    
    pinMode(13, OUTPUT);
    digitalWrite(13, HIGH);
    
    Serial.begin(115200);
    while(!Serial);
    
    strip.begin();
    strip.show();
    
    // Initialise the pixel buffer to zeros
    for(int i=0; i<sizeof(frameBuffer); i++){
	frameBuffer[i] = 0;
    }
    
    if (timeStatus() != timeSet) {
	Serial.printf("%d [ERROR] Unable to sync with the RTC.\n", millis());
    } else {
	if (debug) Serial.printf("%d [DEBUG] RTC has set the system time to %d-%d-%d %d:%d:%d.\n", millis(), year(), month(), day(), hour(), minute(), second());
    }
}



void loop()
{
    // Read the 4 Header Bytes
    if (Serial.available() >= sizeof(header)){
	for(int i=0; i<sizeof(header); i++){
	    header[i] = Serial.read();
	}
	which = 0;
	
	rcvChan  = header[0];
	rcvCmd   = header[1];
	dataSize = header[2] << 8 | header[3];
	
	if(header[0] == 'h' && header[1] == 'e' && header[2] == 'l' &&  header[3] == 'p') help();
	
	match = (dataSize == Serial.available());
	
	// Check that the header matches the packet:
	if (!match){
	    Serial.printf("%d [ERROR] Packet header on channel 0x%X did not match buffer size; Buffer was discarded.\n", millis(), rcvChan);
	    if(debug){
		Serial.printf("%d [DEBUG] Header specified a size of 0x%X (0d%d) bytes, got 0x%X (0d%d).\n", millis(), dataSize, dataSize, Serial.available(), Serial.available());
	    }
	    discardSerial();
	} else if(debug){
	    Serial.printf("%d [DEBUG] Channel: 0x%X Command: 0x%X Data Packet size: %d bytes.\n", millis(), rcvChan, rcvCmd, dataSize);
	}
	
	// If Broadcast or our Channel
	if ((rcvChan == 0 || rcvChan == channel) && match){
	    
	    // Command to write pixels.
	    if (rcvCmd == 0){
		fillBuffer(dataSize);
		writeFrame();
		lock = 0x00;
	    }
	    
	    // System Exclusive Command set
	    if (rcvCmd == 255 && checkID()){
		
		if (debug) Serial.printf("%d [DEBUG] Recieved sysEx command and checked System ID, with %d bytes remaining in Serial buffer.\n", millis(), Serial.available());
		
		/* 
		   So we've taken the header & the first two data bytes (checkID) meaning
		   the remaining bytes in the Serial buffer should be a SysEx command.
		   The first byte determines which setting
		*/
		switch (Serial.read()) {
		    
		    // Set Time
		case 0x01: {
		    time_t tmp = timeSync();
		    if (tmp != 0) {
			setTime(tmp);
			if (debug) Serial.printf("%d [DEBUG] Set time to %d seconds since Epoch.\n", millis(), tmp);
		    } else {
			if (debug) Serial.printf("%d [ERROR] Failed to set a new time - timeSync returned 0.\n", millis());
		    }
		    break;
		}
		    
		    // Get time
		case 0x02:
		    Serial.printf("%d [OUT] Currently held time is %d seconds since UNIX epoch.\n", millis(), now());
		    Serial.printf("%d [OUT] This gives a datetime of %d-%d-%d %d:%d:%d\n", millis(), year(), month(), day(), hour(), minute(), second());
		    break;
		    
		    // Set Channel
		case 0x03:
		    if(Serial.available() == 1){
			channel = Serial.read();
			if (debug) Serial.printf("%d [DEBUG] Changed channel to %d \n", millis(), channel);
		    } else {
			Serial.printf("%d [ERROR] Incorrect number of bytes in buffer, expected one.\n", millis()); 
		    }
		    break;
		    
		    // Get Channel
		case 0x04:
		    Serial.printf("%d [OUT] Currently listening on Channel 0x%X and Broadcast, 0x00\n", millis(), channel);
		    break;
		    
		    // Toggle debug
		case 0x05:
		    debug = !debug;
		    Serial.printf("%d [DEBUG] Toggled debug to: %s.\n", millis(), debug ? "on" : "off");
		    break;
		    
		    // Set Mode
		case 0x06:
		    if(Serial.available() == 1){
			mode = Serial.read();
			if(debug) Serial.printf("%d [DEBUG] Set mode to 0x%X.", millis(), mode);
		    } else {
			Serial.printf("%d [ERROR] Expect one byte after command to set mode", millis());
		    }
		    break;
		    
		    // Get Mode
		case 0x07:
		    Serial.printf("%d [OUT] Current Mode is %d", millis(), mode);
		    break;
		    
		    // Default to nothing
		default:
		    Serial.printf("%d [WARNING] No valid command found in Serial buffer after Header indicated system exclusive command to follow.\n", millis());
		    // Clear buffer given last packet was erroneos
		    discardSerial();
		    break;
		}
	    }
	} else {
	    // Routine if channel wasn't us
	    discardSerial();
	}
    }
    
    
    
    switch (mode) {
    case 1:
	rainbow(40);
	break;
	
    case 2:
	if(lock != 0x01){
	    for(int i=0; i<sizeof(frameBuffer); i+=3){
		frameBuffer[i]   = (byte) colourTemp(temperature) >> 16;
		frameBuffer[i+1] = (byte) colourTemp(temperature) >> 8;
		frameBuffer[i+2] = (byte) colourTemp(temperature);
	    }
	    writeFrame();
	    lock = 0x01;
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
    strip.show();
    if (debug) Serial.printf("%d [DEBUG] Wrote frame at %d:%d:%d.\n", millis(), hour(), minute(), second());
    
    lastframe = now();
}


/** /-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\ **/


void fillBuffer(uint16_t packetSize)
{
    if(packetSize >= sizeof(frameBuffer)) packetSize = sizeof(frameBuffer);
    
    int i=0;
    while( Serial.available() > packetSize ){
	frameBuffer[i] = Serial.read();
	i++; // Next byte in the buffer
	packetSize--; // Remaining bytes to read
    }
    // Discard the remainder of the buffer:
    discardSerial();    
}


/** /-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\ **/

void rainbow(uint8_t wait) 
{
    uint16_t i, j;
    
    for(j=0; j<256*5; j++) { // 5 cycles of all colors on wheel
	for(i=0; i< strip.numPixels(); i++) {
	    strip.setPixelColor(i, Wheel(((i * 256 / strip.numPixels()) + j) & 255));
	}
	strip.show();
	delay(wait);
    }
}

// Input a value 0 to 255 to get a color value.
// The colours are a transition r - g - b - back to r.
uint32_t Wheel(byte WheelPos) {
    if(WheelPos < 85) {
	return strip.Color(WheelPos * 3, 255 - WheelPos * 3, 0);
    } else if(WheelPos < 170) {
	WheelPos -= 85;
	return strip.Color(255 - WheelPos * 3, 0, WheelPos * 3);
    } else {
	WheelPos -= 170;
	return strip.Color(0, WheelPos * 3, 255 - WheelPos * 3);
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
    } else {
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
    } else {
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

// Checks if the first two bytes of the Serial buffer match the System ID
boolean checkID()
{
    if( Serial.available() >= 2){
	if ( (Serial.read() << 8 | Serial.read()) == SYSTEM_ID){
	    return true;
	} else {
	    discardSerial();
	    if (debug) Serial.printf("%d [DEBUG] Incorrect system ID supplied. ID is 0x%X\n", millis(), SYSTEM_ID);
	    return false;
	}
    }
}


/** /-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\ **/


time_t timeSync()
{
    time_t pctime = 0L;
    byte tmp;
    while(Serial.available() > 0) {
	tmp = Serial.read();
	pctime = (pctime << 8) | tmp;
    }
    if (debug) Serial.printf("%d [DEBUG] timeSync read the number %d (0x%X) from Serial.\n", millis(), pctime, pctime);
    if(dtime > pctime){
	pctime = 0L;
    } else {
	// Sync time with teensy's RTC
	Teensy3Clock.set(pctime);
	setTime(pctime);
    }
    return pctime;
}


/** /-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\ **/


time_t getRTCTime()
{
    return Teensy3Clock.get();
}


/** /-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\ **/


void discardSerial()
{
    while(Serial.available() > 0) Serial.read();
}


/** /-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\ **/


void dprint(String message, ...)
{
    va_list list;
    int i = 0;
    
    if(debug) Serial.print(message);
    lastdbg = message;
}


/** /-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\|/-\ **/


void help()
{
    Serial.println(
	"   THIS IS A HELP TEXT, Printed on the input of ASCII 'help'\n"
	"\n"
	"   UNIQUE CASE: 'help' will print a help text. (0x68 0x65 0x6C 0x70)\n"
	"\n"
	"   |-------------|-------------|-------------|--------------------------------|\n"
	"   | Channel     | Command     | Length (n)  | Data                           |\n"
	"   |-------------|-------------|-------------|--------------------------------|\n"
	"   | 0x00 - 0xFF | 0x00 - 0xFE | MSB  |  LSB | n bytes of message data        |\n"
	"   |-------------|-------------|------|------|------------|------------|------|\n"
	"   | 0x00 - 0xFF | 0xFF        | MSB  |  LSB | SysID MSB  |  SysID LSB | Data |\n"
	"   |-------------|-------------|------|------|------------|------------|------|\n"
	"\n"
	"   Command:\n"
	"   0   : 0x00 : Set Pixel Colours\n"
	"   255 : 0xFF : System Exclusive\n"
	"   - The first two bytes of the Data block in the case of the system exclusive\n"
	"     command should be the SYSTEM_ID, unique to this device. This system has ID\n"
	"     0xEDED. The ID 0x0001 is reserved for the FadeCandy.\n"
	"\n"
	"   Length:\n"
	"   Exact length of the Data block, in bytes, range of 0 - 65535.\n"
	"   For command 0 (Set Pixel Colours) the Data block will be 3 * numPixels.\n"
	"\n"
	"   Data:\n"
	"   Contains n bytes of data\n"
	"   If the command was SysEx, the first two bytes are the system ID\n"
	"\n"
	"   SYSTEM EXCLUSIVES:\n"
	"   0x01  :  Set time. Expects UNIX epoch time in bytes.\n"
	"   0x02  :  Get time. Returns current time as a human readable string.\n"
	"   0x03  :  Set channel. Expects channel to listen on as a single byte.\n"
	"   0x04  :  Get channel. Returns current channel (int) as human readable string.\n"
	"   0x05  :  Toggle debug on or off. Informs with Human readable string.\n"
	"   0x06  :  Set the mode. 0=Listen, 1=Light, 2=Spectrum\n"
	"   0x07  :  Get mode. Prints mode (int) to serial\n"
	"\n"
	"   You may disregard any errors as the program will attempt to process 'help'\n"
	);
}