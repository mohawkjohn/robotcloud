#include <Arduino.h>
#include <SPI.h>
#include "Adafruit_BLE.h"
#include "Adafruit_BluefruitLE_SPI.h"
#include "Adafruit_BluefruitLE_UART.h"
#include "/Users/jwoods/Documents/Arduino/FastLED/FastLED.h"

#include "BluefruitConfig.h"

#if SOFTWARE_SERIAL_AVAILABLE
  #include <SoftwareSerial.h>
#endif

/*=========================================================================
    APPLICATION SETTINGS

    FACTORYRESET_ENABLE       Perform a factory reset when running this sketch
   
                              Enabling this will put your Bluefruit LE module
                              in a 'known good' state and clear any config
                              data set in previous sketches or projects, so
                              running this at least once is a good idea.
   
                              When deploying your project, however, you will
                              want to disable factory reset by setting this
                              value to 0.  If you are making changes to your
                              Bluefruit LE device via AT commands, and those
                              changes aren't persisting across resets, this
                              is the reason why.  Factory reset will erase
                              the non-volatile memory where config data is
                              stored, setting it back to factory default
                              values.
       
                              Some sketches that require you to bond to a
                              central device (HID mouse, keyboard, etc.)
                              won't work at all with this feature enabled
                              since the factory reset will clear all of the
                              bonding data stored on the chip, meaning the
                              central device won't be able to reconnect.
    MINIMUM_FIRMWARE_VERSION  Minimum firmware version to have some new features
    MODE_LED_BEHAVIOUR        LED activity, valid options are
                              "DISABLE" or "MODE" or "BLEUART" or
                              "HWUART"  or "SPI"  or "MANUAL"
    -----------------------------------------------------------------------*/
    #define FACTORYRESET_ENABLE         1
    #define MINIMUM_FIRMWARE_VERSION    "0.7.0"
    #define MODE_LED_BEHAVIOUR          "MODE"
/*=========================================================================*/

/// FASTLED defines
#if defined(FASTLED_VERSION) && (FASTLED_VERSION < 3001000)
#warning "Requires FastLED 3.1 or later; check github for latest code."
#endif

#define CLOUD_DATA_PIN    21
#define CLOUD_CLK_PIN     5
#define CLOUD_LED_TYPE    APA102
#define CLOUD_COLOR_ORDER GRB
#define CLOUD_NUM_LEDS    180

#define ROBOT_DATA_PIN    20
#define ROBOT_LED_TYPE    NEOPIXEL
#define ROBOT_COLOR_ORDER GRB
#define ROBOT_NUM_LEDS    180

CRGBArray<CLOUD_NUM_LEDS> cloud_leds;
CRGBArray<ROBOT_NUM_LEDS> robot_leds;
#define FRAMES_PER_SECOND  120

uint8_t brightness = 127;

/////////////

/* ...hardware SPI, using SCK/MOSI/MISO hardware SPI pins and then user selected CS/IRQ/RST */
Adafruit_BluefruitLE_SPI ble(BLUEFRUIT_SPI_CS, BLUEFRUIT_SPI_IRQ, BLUEFRUIT_SPI_RST);

// A small helper
void error(const __FlashStringHelper*err) {
  Serial.println(err);
  while (1);
}


void setup(void) {
  for (int ii = 0; ii < 5; ++ii) {
    if (Serial) break;
    else delay(200);
  }

  Serial.begin(115200);
  Serial.println(F("Robot Cloud"));
  Serial.println(F("Initializing Bluefruit LE module: "));

  if ( !ble.begin(VERBOSE_MODE) )
  {
    error(F("Couldn't find Bluefruit, make sure it's in CoMmanD mode & check wiring?"));
  }
  Serial.println( F("OK!") );

  if ( FACTORYRESET_ENABLE )
  {
    /* Perform a factory reset to make sure everything is in a known state */
    Serial.println(F("Performing a factory reset: "));
    if ( ! ble.factoryReset() ){
      error(F("Couldn't factory reset"));
    }
  }

  // Not totally sure what this does, false has same apparent effect
  ble.echo(true);

  ble.sendCommandCheckOK("AT+GAPDEVNAME=Chandelier");

  Serial.println("Requesting Bluefruit info:");
  /* Print Bluefruit information */
  ble.info();

  /* Set callbacks */
  ble.setConnectCallback(connected);
  ble.setDisconnectCallback(disconnected);
  ble.setBleUartRxCallback(BleUartRX);  

  Serial.println(F("Please use Adafruit Bluefruit LE app to connect in UART mode"));
  Serial.println(F("Then Enter characters to send to Bluefruit"));
  Serial.println();

  ble.verbose(false);  // debug info is a little annoying after this point!

  // LED Activity command is only supported from 0.6.6
  if ( ble.isVersionAtLeast(MINIMUM_FIRMWARE_VERSION) )
  {
    // Change Mode LED Activity
    Serial.println(F("******************************"));
    Serial.println(F("Change LED activity to " MODE_LED_BEHAVIOUR));
    ble.sendCommandCheckOK("AT+HWModeLED=" MODE_LED_BEHAVIOUR);
    Serial.println(F("******************************"));
  } else {
    error( F("Callback requires at least version 0.7.0") );
  }

  delay(3000); // not sure why we need this
  FastLED.addLeds<CLOUD_LED_TYPE,CLOUD_DATA_PIN,CLOUD_CLK_PIN,CLOUD_COLOR_ORDER>(cloud_leds, CLOUD_NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.addLeds<ROBOT_LED_TYPE,ROBOT_DATA_PIN>(robot_leds, ROBOT_NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(brightness);
}

// Use to acknowledge receipt of a command visually
void blinkLED(int n, int dt) {
  for (int ii = 0; ii < n; ++ii) {
    digitalWrite(13, HIGH);
    delay(dt);
    digitalWrite(13, LOW);
    delay(dt);
  }
}


// List of patterns to cycle through.  Each is defined as a separate function below.
typedef void (*SimplePatternList[])();
SimplePatternList CLOUD_PATTERNS = { rainbow, confetti, white }; //, sinelon, juggle, bpm };
SimplePatternList ROBOT_PATTERNS = { processing, angry };

// for fire pattern
//CRGBPalette16 fire_palette= HeatColors_p;
bool fire_reverse_direction = false;

// COOLING: How much does the air cool as it rises?
// Less cooling = taller flames.  More cooling = shorter flames.
// Default 50, suggested range 20-100 
// SPARKING: What chance (out of 255) is there that a new spark will be lit?
// Higher chance = more roaring fire.  Lower chance = more flickery fire.
// Default 120, suggested range 50-200.
int cooling = 80;
int sparking = 50;

uint8_t max_cloud_cycle_index = 1;
uint8_t max_robot_cycle_index = 1;
uint8_t cloud_pattern_index = 0; // Index number of which pattern is current
uint8_t robot_pattern_index = 0;
uint8_t robot_rainbow_hue = 0; // rotating "base color" used by many of the patterns
uint8_t cloud_rainbow_hue = 0;
fract8 chance_of_waking = 40;
bool cycle_cloud_patterns = true; 
bool cycle_robot_patterns = true;
bool enable_glitter = false;
bool enable_coma    = false;
bool enable_sleep   = false;



void connected(void)
{
  Serial.println( F("Connected") );
}

void disconnected(void)
{
  Serial.println( F("Disconnected") );
}


void respond(char* data, bool ble_also = true) {
  Serial.println(data);
  if (ble_also) {
    ble.print("AT+BLEUARTTX=");
    ble.println(data);
  }
}


void BleUartRX(char data[], uint16_t len)
{
  Serial.print( F("[BLE UART RX]" ) );
  Serial.write(data, len);
  Serial.println();
  
  if (strcmp(data, "cycle") == 0) {
    cycle_cloud_patterns = true;
    cycle_robot_patterns = true;
    respond("cycling patterns");
    ble.waitForOK();
  } else if (strcmp(data, "glitter") == 0) {
    if (enable_glitter) enable_glitter = false;
    else                enable_glitter = true;
    respond("toggling glitter");
    ble.waitForOK();
  } else if (strcmp(data, "rainbow") == 0) {
    respond("rainbow mode");
    cycle_cloud_patterns = false;
    cloud_pattern_index = 0;
    ble.waitForOK();
  } else if (strcmp(data, "confetti") == 0) {
    respond("confetti mode");
    cycle_cloud_patterns = false;
    cloud_pattern_index = 1;
    ble.waitForOK();
  } else if (strcmp(data, "white") == 0) {
    respond("white mode");
    cycle_cloud_patterns = false;
    cloud_pattern_index = 2;
    ble.waitForOK();
  } else if (strcmp(data, "sleep") == 0) {
    respond("putting robots to sleep");
    cooling = 100;
    sparking = 30;    
    enable_sleep = true;
  } else if (strcmp(data, "coma") == 0) {
    respond("inducing robotic coma");
    cooling = 100;
    sparking = 10;
    enable_coma = true;
  } else if (strcmp(data, "wake") == 0) {
    respond("number five is alive!");
    enable_coma = false;
    enable_sleep = false;
    cooling = 80;
    sparking = 50;
  } else if (strcmp(data, "angry") == 0) {
    respond("i'm opening the pod bay doors, dave");
    robot_pattern_index = 1;
    cycle_robot_patterns = false;
    enable_coma = false;
    enable_sleep = false;
  } else if (strcmp(data, "lull") == 0) {
    robot_pattern_index = 0;
    cycle_robot_patterns = false;
    enable_coma = false;
    enable_sleep = false;
  } else if (data[0] == 'b') {
    brightness = atoi(&(data[1]));
    if (brightness >= 0 && brightness < 256) {
      respond("brightness changed");
      FastLED.setBrightness(brightness);
    } else {
      respond("brightness setting must be between 0 and 255 inclusive");
    }
  } else {
    if (strcmp(data, "help") != 0) {
      respond("Invalid command! ");
    }
    respond("COMMANDS:");
    respond(" b# (set brightness (0-255)) ");
    respond(" rainbow confetti glitter coma sleep angry lull");
    ble.waitForOK();
  }
}

void loop() {
  CLOUD_PATTERNS[cloud_pattern_index]();
  ROBOT_PATTERNS[robot_pattern_index]();
  
  if (enable_glitter) add_glitter(80);
  flash_eyes(90);
  
  // send the 'leds' array out to the actual LED strip
  FastLED.show();
  // insert a delay to keep the framerate modest
  FastLED.delay(1000 / FRAMES_PER_SECOND);

  // do some periodic updates
  EVERY_N_MILLIS_I(cloud_hue_timer, 10) { cloud_rainbow_hue++; } // cycle rainbow base color (this line sets rate)
  EVERY_N_MILLIS_I(robot_hue_timer, 100) {
    if (enable_coma) robot_hue_timer.setPeriod( 100 );
    else if (enable_sleep) robot_hue_timer.setPeriod( 50 );
    else robot_hue_timer.setPeriod( 1 );
    robot_rainbow_hue++;
  }
  
  if (cycle_cloud_patterns) {
    EVERY_N_SECONDS_I( cloud_cycle_timer, 10 ) {
      Serial.print("Cycling cloud\n");
      next_cloud_pattern();
    }
  }
  if (cycle_robot_patterns) {
    EVERY_N_SECONDS_I( robot_cycle_timer, 9 ) {
      Serial.print("Cycling robot\n");
      next_robot_pattern(); 
    }
  }

  ble.update(200);
  
}

#define ARRAY_SIZE(A) (sizeof(A) / sizeof((A)[0]))

void next_robot_pattern(void) {
  // add one and wrap around
  robot_pattern_index = (robot_pattern_index + 1) % max_robot_cycle_index;
}

void next_cloud_pattern(void) {
  // add one and wrap around
  cloud_pattern_index = (cloud_pattern_index + 1) % max_cloud_cycle_index;
}

void add_glitter( fract8 chance_of_glitter) 
{
  if( random8() < chance_of_glitter) {
    if (robot_pattern_index == 2)
      robot_leds[ random16(ROBOT_NUM_LEDS) ] = CRGB::Black;
    else
      robot_leds[ random16(ROBOT_NUM_LEDS) ] += CRGB::White;
      

    if (cloud_pattern_index == 2)
      cloud_leds[ random16(CLOUD_NUM_LEDS) ] = CRGB::Black;
    else
      cloud_leds[ random16(CLOUD_NUM_LEDS) ] += CRGB::White; 
  }

}

void rainbow() {
  // FastLED's built-in rainbow generator
  fill_rainbow( cloud_leds, CLOUD_NUM_LEDS, cloud_rainbow_hue, 7 );
}

void white() {
  for (int ii = 0; ii < CLOUD_NUM_LEDS; ++ii) {
    cloud_leds[ii] += CRGB::White;
  }
}

/*
void black() {
  for (int ii = 0; ii < CLOUD_NUM_LEDS; ++ii) {
    cloud_leds[ii] = CRGB::Black;
  }
  for (int ii = 0; ii < ROBOT_NUM_LEDS; ++ii) {
    robot_leds[ii] = CRGB::Black;
  }
}*/

void confetti() 
{
  // random colored speckles that blink in and fade smoothly
  cloud_leds.fadeToBlackBy(10);
  int pos = random16(CLOUD_NUM_LEDS);
  cloud_leds[pos] += CHSV( cloud_rainbow_hue + random8(64), 200, 255);
}


#define NUM_A 60
#define NUM_B 60
#define NUM_C 60
#define BRAIN_A_OFFSET 44
#define BRAIN_B_OFFSET 10
#define BRAIN_C_OFFSET 0
#define EYE_C1 NUM_A + NUM_B + 2
#define EYE_C2 NUM_A + NUM_B + 57

void angry() {
  int8_t fade_rate = 10;
  if (enable_sleep || enable_coma) fade_rate = 96;
  
  robot_leds(BRAIN_A_OFFSET, NUM_A -1).fadeToBlackBy(fade_rate);
  robot_leds(NUM_A + BRAIN_B_OFFSET, NUM_A + NUM_B - 1).fadeToBlackBy(fade_rate);
  robot_leds(NUM_A + NUM_C, NUM_A + NUM_B + NUM_C - 1).fadeToBlackBy(fade_rate);
  if (!enable_coma || random8() < chance_of_waking) {
    int pos_a = random16(NUM_A - BRAIN_A_OFFSET);
    int pos_b = random16(NUM_B - BRAIN_B_OFFSET);
    int pos_c = random16(NUM_C - BRAIN_C_OFFSET);
    robot_leds[BRAIN_A_OFFSET + pos_a] = CRGB::Red;
    robot_leds[NUM_A + BRAIN_B_OFFSET + pos_b] = CRGB::Red;
    robot_leds[NUM_A + NUM_B + pos_c] = CRGB::Red;
  }

  fire();
}

void confetti_brain() {
  int8_t fade_rate = 10;
  if (enable_sleep || enable_coma) fade_rate = 96;
  
  robot_leds(BRAIN_A_OFFSET, NUM_A - 1).fadeToBlackBy(fade_rate);
  robot_leds(NUM_A + BRAIN_B_OFFSET, NUM_A + NUM_B - 1).fadeToBlackBy(fade_rate);
  robot_leds(NUM_A + NUM_B, NUM_A + NUM_B + NUM_C - 1).fadeToBlackBy(fade_rate);

  if (!enable_coma || random8() < chance_of_waking) {
    int pos_a = random16(NUM_A - BRAIN_A_OFFSET);
    int pos_b = random16(NUM_B - BRAIN_B_OFFSET);
    int pos_c = random16(NUM_C - BRAIN_C_OFFSET);
    robot_leds[BRAIN_A_OFFSET + pos_a] += CHSV( robot_rainbow_hue + random8(64), 200, 255);
    robot_leds[NUM_A + BRAIN_B_OFFSET + pos_b] += CHSV( robot_rainbow_hue + random8(64), 200, 255);
    robot_leds[NUM_A + NUM_B + pos_c] += CHSV( robot_rainbow_hue + random8(64), 200, 255);
  }
}


void rainbow_robot() {
  robot_leds(0, BRAIN_A_OFFSET-1).fill_rainbow( robot_rainbow_hue, 7 );
  robot_leds(NUM_A, NUM_A + BRAIN_B_OFFSET-1).fill_rainbow( robot_rainbow_hue, 7 );
  //robot_leds(119, 120).fill_rainbow(robot_rainbow_hue, 7);
}


void flash_eyes(fract8 chance_of_anger) {
  robot_leds(NUM_A + NUM_B + BRAIN_A_OFFSET, NUM_A + NUM_B + NUM_C - 1).fadeToBlackBy(80);
  if ( random8() > chance_of_anger ) {
    robot_leds[EYE_C1] = CRGB::Red;
  }
  if (random8() > chance_of_anger ) {
    robot_leds[EYE_C2] = CRGB::Red;
  }
}

void processing() {
  confetti_brain();
  rainbow_robot();
}

// COOLING: How much does the air cool as it rises?
// Less cooling = taller flames.  More cooling = shorter flames.
// Default 50, suggested range 20-100 
#define COOLING  80

// SPARKING: What chance (out of 255) is there that a new spark will be lit?
// Higher chance = more roaring fire.  Lower chance = more flickery fire.
// Default 120, suggested range 50-200.
#define SPARKING 50


void fire2012(int i_offset, int n)
{
// Array of temperature readings at each simulation cell
  static byte heat[ROBOT_NUM_LEDS];

  // Step 1.  Cool down every cell a little
  for( int i = 0; i < n - i_offset; i++) {
    heat[i] = qsub8( heat[i],  random8(0, ((COOLING * 10) / n) + 2));
  }
  
  // Step 2.  Heat from each cell drifts 'up' and diffuses a little
  for( int k = n - i_offset - 1; k >= 2; k--) {
    heat[k] = (heat[k - 1] + heat[k - 2] + heat[k - 2] ) / 3;
  }
    
  // Step 3.  Randomly ignite new 'sparks' of heat near the bottom
  if( random8() < SPARKING ) {
    int y = random8(7);
    heat[y] = qadd8( heat[y], random8(160,255) );
  }

  // Step 4.  Map from heat cells to LED colors
  for( int j = 0; j < n - i_offset; j++) {
    CRGB color = HeatColor( heat[j]);
    int pixelnumber;
    if( fire_reverse_direction ) {
      pixelnumber = i_offset + (n-1) - j;
    } else {
      pixelnumber = i_offset + j;
    }
    robot_leds[pixelnumber] = color;
  }
}


void fire() {
  if (!enable_coma) {
    fire2012(0, BRAIN_A_OFFSET);
    fire2012(NUM_A, NUM_A + BRAIN_B_OFFSET);
    if (BRAIN_C_OFFSET > 0)
      fire2012(NUM_A + NUM_B, NUM_A + NUM_B + BRAIN_C_OFFSET);
  }
}

