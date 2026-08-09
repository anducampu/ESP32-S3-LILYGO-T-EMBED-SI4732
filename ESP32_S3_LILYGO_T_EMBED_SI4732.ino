/*
  This sketch runs on Lilygo T-Display S3 device (and also T-Embed by changing Pins).

  It is  a  complete  radio  capable  to  tune  LW,  MW,  SW  on  AM  and  SSB  mode  and  also  receive  the
  regular  comercial  stations.

  Features:   AM; SSB; LW/MW/SW; external mute circuit control; AGC; Attenuation gain control;
              SSB filter; CW; AM filter; 1, 5, 10, 50 and 500kHz step on AM and 10Hhz sep on SSB

  Lilygo T-Display S3
  
    |--------------|------------|------------|------------|
    |   Lilygo     |   Si4735   |  Encoder   |   Audio    |
    | T-Display S3 |            |            | Amplifier  |        
    |--------------|------------|------------|------------|        
    |     3V3      |    Vcc     |            |    Vcc     |        
    |     GND      |    GND     |     2,4    |    GND     |        Encoder        1,2,3        
    |     21       |            |     5      |            |        Encoder switch 4,5
    |     16       |   Reset    |            |            |
    |     43       |    SDA     |            |            |
    |     44       |    SCL     |            |            |
    |      1       |            |      1     |            |
    |      2       |            |      3     |            |
    |              |    LOut    |            |    LIn     |
    |              |    ROut    |            |    RIn     |
    |     17 Mute  |            |            |    Mute    |
    |--------------|------------|------------|------------|
  
  
    Lilygo T-Embed
  
    |--------------|------------|------------|
    |    Lilygo    |   Si4735   |   Audio    |
    |   T-Embeded  |            | Amplifier  |
    |--------------|------------|------------|
    |     3V3      |    Vcc     |    Vcc     |
    |     GND      |    GND     |    GND     |
    |              |            |            |
    |     16       |   Reset    |            |
    |     18       |    SDA     |            |
    |      8       |    SCL     |            |
    |              |            |            |
    |              |            |            |
    |              |    LOut    |    LIn     |
    |              |    ROut    |    RIn     |
    |     17 Mute  |            |    Mute    |
    |--------------|------------|------------|
  
  (*1) If you are using the SI4732-A10, check the corresponding pin numbers.
  (*2) If you are using the Lilygo T-Embeded, check the corresponding pin numbers.
  (*3) The PU2CLR SI4735 Arduino Library has resources to detect the I2C bus address automatically.
       It seems the original project connect the SEN pin to the +Vcc. By using this sketch, you do
       not need to worry about this setting.
  Prototype documentation: https://pu2clr.github.io/SI4735/
  PU2CLR Si47XX API documentation: https://pu2clr.github.io/SI4735/extras/apidoc/html/

  By PU2CLR, Ricardo, May  2021.
  Modded by Ralph Xavier, Jan 2023
*/

// SI4735_Shield example form https://github.com/ralphxavier/SI4735
#include <Wire.h>
#include <TFT_eSPI.h>
#include "EEPROM.h"
#include <Preferences.h>   // to clear the CAT / WiFi namespaces on a config reset
#include <SI4735.h>
#include <Battery18650Stats.h> // https://github.com/danilopinotti/Battery18650Stats
#include <OneButton.h>  // https://github.com/mathertel/OneButton

#include "Rotary.h"
#include "patch_init.h" // SSB patch for whole SSBRX initialization string
#define FASTLED_INTERNAL  // silence FastLED's pragma message
// FastLED 3.10.x logs its channel-driver registration at runtime, through Serial,
// from inside FastLED.addLeds(). That is the USB CDC port CAT owns, and BOOTLOG_LN
// cannot gate a print made from inside a library -- so an enabled CAT USB transport
// got ten lines of "ChannelManager: Added driver ..." injected into its command
// stream on every boot (observed on hardware).
//
// The switch is FASTLED_LOG_VERBOSITY=0, but it cannot live here: Arduino compiles
// each library into its own translation units, which never see the sketch's defines.
// It is in build_opt.h instead, which the builder applies to the whole build.
#include <FastLED.h>
#include <driver/rtc_io.h>  // to hand GPIO0 back from RTC mode after deep sleep
#include "CatControl.h"  // Kenwood TS-480 style CAT: USB serial + WiFi TCP
#include "WiFiPortal.h"  // multi-network WiFi store + AP captive portal

const uint16_t size_content = sizeof ssb_patch_content; // see patch_init.h

#define FM_BAND_TYPE 0
#define MW_BAND_TYPE 1
#define SW_BAND_TYPE 2
#define LW_BAND_TYPE 3

#define PIN_POWER_ON  46
#define PIN_LCD_BL    15
#define RESET_PIN     16           
#define AUDIO_MUTE    17           

// Enconder PINs
#define ENCODER_PIN_A  2           // GPIO01 
#define ENCODER_PIN_B  1           // GPIO02

// I2C bus pin on Lilygo T-Display
#define ESP32_I2C_SDA 18           // GPIO43 
#define ESP32_I2C_SCL 8           // GPIO44

//Battery Monitor
#define VBAT_MON         4                // GPIO04
#define MIN_USB_VOLTAGE  4.9
#define CONV_FACTOR      1.8
#define READS            20

// Buttons controllers
#define ENCODER_PUSH_BUTTON     0     // GPIO21

// APA102 RGB LED ring around the encoder (T-Embed SI4732 pin map)
#define APA102_DATA_PIN   42
#define APA102_CLK_PIN    45
#define APA102_NUM_LEDS    7

// Screen-saver timeout: turn the backlight off after this much idle time
#define SCREEN_TIMEOUT_MS 20000UL

#define MIN_ELAPSED_TIME         5  //300
#define MIN_ELAPSED_RSSI_TIME  200
#define ELAPSED_COMMAND       2000  // time to turn off the last command controlled by encoder. Time to goes back to the FVO control
#define ELAPSED_CLICK         1500  // time to check the double click commands
#define DEFAULT_VOLUME          35  // change it for your favorite sound volume
#define STRENGTH_CHECK_TIME   1500
#define RDS_CHECK_TIME          90

#define FM  0
#define LSB 1
#define USB 2
#define AM  3
#define LW  4

#define SSB 1

#define VOLUME       0
#define STEP         1
#define MODE         2
#define BFO          3 
#define BW           4
#define AGC_ATT      5
#define SOFTMUTE     6
#define SEEKUP       7
#define SEEKDOWN     8
#define BAND         9
#define MUTE        10
#define CAT_USB     11
#define CAT_WIFI    12
#define WIFI_CFG    13
#define RESET_CFG   14

#define TFT_MENU_BACK TFT_BLACK  // 0x01E9
#define TFT_MENU_HIGHLIGHT_BACK TFT_BLUE

#define EEPROM_SIZE        512

#define STORE_TIME 10000 // Time of inactivity to make the current receiver status writable (10s / 10000 milliseconds).

// EEPROM - Stroring control variables
const uint8_t app_id = 47; // Useful to check the EEPROM content before processing useful data
const int eeprom_address = 0;
long storeTime = millis();

bool itIsTimeToSave = false;

bool bfoOn = false;
bool ssbLoaded = false;
char bfo[18]="0000";
bool muted = false;
int8_t agcIdx = 0;
uint8_t disableAgc = 0;
int8_t agcNdx = 0;
int8_t softMuteMaxAttIdx = 4;
uint8_t countClick = 0;

uint8_t seekDirection = 1;

bool cmdBand = false;
bool cmdVolume = false;
bool cmdAgc = false;
bool cmdBandwidth = false;
bool cmdStep = false;
bool cmdMode = false;
bool cmdMenu = false;
bool cmdSoftMuteMaxAtt = false;

bool fmRDS = false;

int16_t currentBFO = 0;
long elapsedRSSI = millis();
long elapsedButton = millis();

long lastStrengthCheck = millis();
long lastRDSCheck = millis();

long elapsedClick = millis();
long elapsedCommand = millis();
volatile int encoderCount = 0;
uint16_t currentFrequency;

const uint8_t currentBFOStep = 10;

char sAgc[15];

const char *menu[] = {"Volume", "Step", "Mode", "BFO", "BW", "AGC/Att", "SoftMute", "Seek Up", "Seek Dn", "Band", "Mute", "CAT USB", "CAT WiFi", "WiFi Cfg", "Reset Cfg"};
int8_t menuIdx = VOLUME;
const int lastMenu = (sizeof menu / sizeof(char *)) - 1;
int8_t currentMenuCmd = -1;

typedef struct
{
  uint8_t idx;      // SI473X device bandwidth index
  const char *desc; // bandwidth description
} Bandwidth;

int8_t bwIdxSSB = 4;
const int8_t maxSsbBw = 5;
Bandwidth bandwidthSSB[] = {
  {4, "0.5"},
  {5, "1.0"},
  {0, "1.2"},
  {1, "2.2"},
  {2, "3.0"},
  {3, "4.0"}
};
const int lastBandwidthSSB = (sizeof bandwidthSSB / sizeof(Bandwidth)) - 1;

int8_t bwIdxAM = 4;
const int8_t maxAmBw = 6;
Bandwidth bandwidthAM[] = {
  {4, "1.0"},
  {5, "1.8"},
  {3, "2.0"},
  {6, "2.5"},
  {2, "3.0"},
  {1, "4.0"},
  {0, "6.0"}
};
const int lastBandwidthAM = (sizeof bandwidthAM / sizeof(Bandwidth)) - 1;

int8_t bwIdxFM = 0;
const int8_t maxFmBw = 4;

Bandwidth bandwidthFM[] = {
    {0, "AUT"}, // Automatic - default
    {1, "110"}, // Force wide (110 kHz) channel filter.
    {2, " 84"},
    {3, " 60"},
    {4, " 40"}};
const int lastBandwidthFM = (sizeof bandwidthFM / sizeof(Bandwidth)) - 1;



int tabAmStep[] = {1,    // 0
                   5,    // 1
                   9,    // 2
                   10,   // 3
                   50,   // 4
                   100}; // 5

const int lastAmStep = (sizeof tabAmStep / sizeof(int)) - 1;
int idxAmStep = 3;

int tabFmStep[] = {5, 10, 20};
const int lastFmStep = (sizeof tabFmStep / sizeof(int)) - 1;
int idxFmStep = 1;

uint16_t currentStepIdx = 1;


const char *bandModeDesc[] = {"FM ", "LSB", "USB", "AM "};
const int lastBandModeDesc = (sizeof bandModeDesc / sizeof(char *)) - 1;
uint8_t currentMode = FM;


/**
 *  Band data structure
 */
typedef struct
{
  const char *bandName;   // Band description
  uint8_t bandType;       // Band type (FM, MW or SW)
  uint16_t minimumFreq;   // Minimum frequency of the band
  uint16_t maximumFreq;   // maximum frequency of the band
  uint16_t currentFreq;   // Default frequency or current frequency
  int8_t currentStepIdx;  // Idex of tabStepAM:  Defeult frequency step (See tabStepAM)
  int8_t bandwidthIdx;    // Index of the table bandwidthFM, bandwidthAM or bandwidthSSB;
} Band;

/*
   Band table
   YOU CAN CONFIGURE YOUR OWN BAND PLAN. Be guided by the comments.
   To add a new band, all you have to do is insert a new line in the table below. No extra code will be needed.
   You can remove a band by deleting a line if you do not want a given band. 
   Also, you can change the parameters of the band.
   ATTENTION: You have to RESET the eeprom after adding or removing a line of this table. 
              Turn your receiver on with the encoder push button pressed at first time to RESET the eeprom content.  
*/
Band band[] = {
    {"VHF", FM_BAND_TYPE, 6400, 10800, 10390, 1, 0},
    {"MW1", MW_BAND_TYPE, 150, 1720, 810, 3, 4},
    {"MW2", MW_BAND_TYPE, 531, 1701, 783, 2, 4},
    {"MW2", MW_BAND_TYPE, 1700, 3500, 2500, 1, 4},
    {"80M", MW_BAND_TYPE, 3500, 4000, 3700, 0, 4},
    {"SW1", SW_BAND_TYPE, 4000, 5500, 4885, 1, 4},
    {"SW2", SW_BAND_TYPE, 5500, 6500, 6000, 1, 4},
    {"40M", SW_BAND_TYPE, 6500, 7300, 7100, 0, 4},
    {"SW3", SW_BAND_TYPE, 7200, 8000, 7200, 1, 4},
    {"SW4", SW_BAND_TYPE, 9000, 11000, 9500, 1, 4},
    {"SW5", SW_BAND_TYPE, 11100, 13000, 11900, 1, 4},
    {"SW6", SW_BAND_TYPE, 13000, 14000, 13500, 1, 4},
    {"20M", SW_BAND_TYPE, 14000, 15000, 14200, 0, 4},
    {"SW7", SW_BAND_TYPE, 15000, 17000, 15300, 1, 4},
    {"SW8", SW_BAND_TYPE, 17000, 18000, 17500, 1, 4},
    {"15M", SW_BAND_TYPE, 20000, 21400, 21100, 0, 4},
    {"SW9", SW_BAND_TYPE, 21400, 22800, 21500, 1, 4},
    {"CB ", SW_BAND_TYPE, 26000, 28000, 27500, 0, 4},
    {"10M", SW_BAND_TYPE, 28000, 30000, 28400, 0, 4},
    {"ALL", SW_BAND_TYPE, 150, 30000, 15000, 0, 4} // All band. LW, MW and SW (from 150kHz to 30MHz)
};                                             

const int lastBand = (sizeof band / sizeof(Band)) - 1;
int bandIdx = 0;

// The last AM-type (non-FM) band that was in use. A remote "switch to AM" while the
// radio sits on the VHF band has to land somewhere, and returning to whatever the
// user last listened to below 30 MHz beats picking a fixed default.
int8_t lastAmBandIdx = 1;   // MW1 until something better is known
int tabStep[] = {1, 5, 10, 50, 100, 500, 1000};
const int lastStep = (sizeof tabStep / sizeof(int)) - 1;

char *rdsMsg;
char *stationName;
char *rdsTime;
char bufferStationName[50];
char bufferRdsMsg[100];
char bufferRdsTime[32];

uint8_t rssi = 0;
uint8_t snr = 0;
uint8_t volume = DEFAULT_VOLUME;

int XbatPos = 290;  // Position of battery icon
int YbatPos =   6;
int Xbatsiz =  20;  // size of battery icon
int Ybatsiz =   9;
int previousBatteryLevel = -1;
int currentBatteryLevel = 1;
bool batteryCharging = false;

// Devices class declarations
Rotary encoder = Rotary(ENCODER_PIN_A, ENCODER_PIN_B);

Battery18650Stats battery(VBAT_MON);

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);

SI4735 rx;

// --- Screen timeout + APA102 LED ring state ---------------------------------
// Physical-to-logical sort order for the 7-LED ring around the encoder, taken
// from the LilyGO T-Embed factory example. leds[LED_SORT[i]] is the i-th LED
// going around the ring.
const uint8_t LED_SORT[APA102_NUM_LEDS] = {2, 1, 0, 6, 5, 4, 3};
CRGB leds[APA102_NUM_LEDS];

volatile unsigned long lastInteraction = 0;  // updated from main loop only
bool   screenOn          = true;

// LED-ring chase state. ledAnimStart is set only on transitions (idle->active
// or direction change) so the comet position keeps advancing smoothly even
// when encoder ticks come in faster than one step. ledLastActivity is bumped
// on every tick and drives the stop timeout: the chase keeps running for one
// full revolution past the last tick before turning off.
bool          ledActive       = false;
int8_t        ledDir          = 0;           // +1 = right/CW chase, -1 = left/CCW chase, 0 = idle
unsigned long ledAnimStart    = 0;           // when the current chase began (free-running)
unsigned long ledLastActivity = 0;           // when the last encoder tick arrived
#define LED_STEP_MS 70U

// Wake the screen and reset the idle timer. Call this from anywhere a user
// interaction is detected (encoder rotation or button press).
inline void noteInteraction()
{
  lastInteraction = millis();
  if (!screenOn) {
    ledcWrite(PIN_LCD_BL, 255);
    screenOn = true;
  }
}

void updateScreenTimeout()
{
  if (screenOn && (millis() - lastInteraction) > SCREEN_TIMEOUT_MS) {
    ledcWrite(PIN_LCD_BL, 0);
    screenOn = false;
  }
}

// Note an encoder tick in direction `dir`. Cheap; safe to call every tick.
// The animation start time is reset only when transitioning from idle or
// reversing direction, so the chase position keeps free-running through
// rapid ticks instead of restarting on every one.
inline void triggerLedChase(int8_t dir)
{
  unsigned long now = millis();
  ledLastActivity = now;
  if (!ledActive || ledDir != dir) {
    ledAnimStart = now;
    ledDir       = dir;
    ledActive    = true;
  }
}

// Drive the LED ring. Call frequently from loop(); internally throttled.
void updateLedRing()
{
  static unsigned long lastShow = 0;
  unsigned long now = millis();
  if ((now - lastShow) < 30) return;   // throttle to ~33 fps
  lastShow = now;

  if (!ledActive) return;              // idle: nothing to do, near-zero cost

  // Keep animating for one full revolution past the last encoder tick. This
  // lets the comet "complete the rotation" gracefully instead of cutting off
  // mid-step, while a continuous fast spin just keeps it alive indefinitely.
  const unsigned long ONE_REV_MS = LED_STEP_MS * (unsigned long)APA102_NUM_LEDS;
  if ((now - ledLastActivity) > ONE_REV_MS) {
    FastLED.clear(true);
    ledActive = false;
    ledDir    = 0;
    return;
  }

  const int N = APA102_NUM_LEDS;
  uint8_t step = ((now - ledAnimStart) / LED_STEP_MS) % N;
  int pos    = (ledDir > 0) ? step : (N - 1 - step);
  int trail1 = ((pos - ledDir)     % N + N) % N;
  int trail2 = ((pos - 2 * ledDir) % N + N) % N;

  FastLED.clear();
  leds[LED_SORT[pos]]    = CRGB(0, 120, 0);   // head — bright green
  leds[LED_SORT[trail1]] = CRGB(0,  35, 0);
  leds[LED_SORT[trail2]] = CRGB(0,  10, 0);
  FastLED.show();
}
// ---------------------------------------------------------------------------

// The boot diagnostics share the USB CDC port with the CAT transport, so they
// have to fall silent once CAT owns the stream or they corrupt every reply.
#define BOOTLOG_LN(msg)  do { if (!catUsbOwnsStream()) Serial.println(msg); } while (0)
#define BOOTLOG(...)     do { if (!catUsbOwnsStream()) Serial.printf(__VA_ARGS__); } while (0)

// --- encoder push button (GPIO0) ------------------------------------------
// GPIO0 is three things at once here: the encoder push button, the ESP32-S3 boot
// strapping pin, and the deep-sleep ext0 wake source. esp_sleep_enable_ext0_wakeup()
// switches it to RTC mode, and coming back from deep sleep it stays there with no
// pull-up, so pinMode(INPUT_PULLUP) has no effect and digitalRead() reports LOW
// forever. That looked exactly like "button held": the EEPROM got wiped at boot,
// the press handler blocked so the encoder went dead, and the 3 s hold put the
// radio straight back to sleep -- on every single boot.
//
// Belt and braces: releaseEncoderButtonPin() hands the pin back to the digital
// GPIO matrix, and encoderButtonPressed() refuses to trust a LOW that was never
// preceded by a HIGH, latching a fault if it stays low from boot. A bad GPIO0
// then costs the button only, not the whole radio.
// "Reset Cfg" opens a modal instead of arming a second click. The two-click
// version was defeated by the UI's own double-click gesture -- both clicks route
// through doCurrentMenuCmd(), so a double-click armed and confirmed inside ~500 ms
// -- and by the 2 s ELAPSED_COMMAND teardown, which erased the prompt while the
// flag stayed armed for 5 s, so a later click erased everything with nothing shown.
// This modal defaults to Cancel, is drawn from drawSprite() so no redraw can hide
// it, and swallows the encoder and button while it is up.
static bool     cmdConfirmReset = false;
static bool     confirmEraseSel = false;   // false = Cancel
static bool     btnSeenReleased = false;
static bool     btnFaulty       = false;
static uint32_t btnLowSince     = 0;

#define BTN_STUCK_MS 4000UL

static void releaseEncoderButtonPin()
{
  // Both holds matter and they live in different registers: gpio_hold_dis()
  // covers the digital IO hold, rtc_gpio_hold_dis() the RTC domain one. GPIO0 is
  // RTC_GPIO0, and deep sleep parks it in the RTC domain, so releasing only the
  // digital hold left the pull-up inert -- which is exactly what ZZB measured:
  // lvl=0 with the boot-time pull-up, lvl=1 the moment it was re-applied.
  rtc_gpio_hold_dis((gpio_num_t)ENCODER_PUSH_BUTTON);
  rtc_gpio_deinit((gpio_num_t)ENCODER_PUSH_BUTTON);
  gpio_hold_dis((gpio_num_t)ENCODER_PUSH_BUTTON);
}

// True only while the button is genuinely pressed.
static bool encoderButtonPressed()
{
  if (digitalRead(ENCODER_PUSH_BUTTON) == HIGH)
  {
    // A high proves the pin can rise, so any earlier stuck-low verdict was
    // wrong or temporary. Clear it: the previous version latched btnFaulty for
    // the whole session, so one bad moment at boot killed the button until the
    // next power cycle.
    btnSeenReleased = true;
    btnFaulty = false;
    btnLowSince = 0;
    return false;
  }

  if (btnFaulty) return false;

  // Time every continuous low, not just one that started at boot. Gating this on
  // !btnSeenReleased meant a pin that went stuck after a clean boot read as held
  // forever: the press handler span, CAT and the portal stopped being serviced,
  // and at 3 s it deep-slept on a pin already low -- with the BTN? warning never
  // shown because this was the only place that could set it.
  if (!btnLowSince)
  {
    btnLowSince = millis();
    // Re-assert the pull-up once: measurement showed the boot-time configuration
    // can fail to stick while a later pinMode() works.
    releaseEncoderButtonPin();
    pinMode(ENCODER_PUSH_BUTTON, INPUT_PULLUP);
  }
  if (millis() - btnLowSince > BTN_STUCK_MS)
  {
    btnFaulty = true;
    return false;
  }

  // No proof yet that this pin can rise, so do not report a press.
  if (!btnSeenReleased) return false;
  return true;
}

// Cleared when the SI4735 never answered on I2C. Every radio hook that would touch
// the chip consults it, so the diagnostic halt in setup() can keep CAT answering
// without a client being able to drive a receiver that is not there -- a CAT set
// would otherwise retune over a dead bus and repaint the panel straight over the
// "Si4735 not detected" message the halt exists to display.
static bool     radioPresent = true;

// Set when a CAT command changed something the display shows. The redraw is
// rate-limited in loop() so a chatty client cannot monopolise the sprite push.
static bool     catUiDirty = false;
static uint32_t catUiLastDraw = 0;

// The network banner is shown while the setup portal is up, and for a few
// seconds either side of a link state change so the address is readable.
static NetState catLastNetState = NET_OFF;
static uint32_t catNetBannerUntil = 0;

void setup()
{
  Serial.begin(115200);

  // Load the CAT settings first: BOOTLOG below has to know whether the USB
  // transport owns the port. This touches NVS only, never the radio. It also
  // sets the CDC write timeout to match whoever owns the stream.
  catBegin();
  delay(1500);
  BOOTLOG_LN("\n[BOOT] setup() entered");

  pinMode(PIN_POWER_ON, OUTPUT);
  digitalWrite(PIN_POWER_ON, HIGH);
  BOOTLOG_LN("[BOOT] PIN_POWER_ON high");

  // Encoder pins. Take GPIO0 back from RTC mode first, or a previous deep sleep
  // leaves it stuck reading LOW no matter what pinMode() asks for.
  releaseEncoderButtonPin();
  pinMode(ENCODER_PUSH_BUTTON, INPUT_PULLUP);
  delay(20);   // let the pull-up win against the strapping pin's external RC

  pinMode(ENCODER_PIN_A, INPUT_PULLUP);
  pinMode(ENCODER_PIN_B, INPUT_PULLUP);
  BOOTLOG_LN("[BOOT] encoder pins set");

  // The line below may be necessary to setup I2C pins on ESP32
  Wire.begin(ESP32_I2C_SDA, ESP32_I2C_SCL);
  BOOTLOG_LN("[BOOT] Wire.begin done");

  tft.begin();
  BOOTLOG_LN("[BOOT] tft.begin done");

  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);
  BOOTLOG_LN("[BOOT] tft fill black done");
  uint8_t* sp = (uint8_t*)spr.createSprite(320,170);
  BOOTLOG("[BOOT] createSprite ptr=%p free_psram=%u free_heap=%u\n",
                sp, (unsigned)ESP.getFreePsram(), (unsigned)ESP.getFreeHeap());
  spr.setTextDatum(MC_DATUM);
  spr.setSwapBytes(true);
  spr.setFreeFont(&Orbitron_Light_24);
  spr.setTextColor(TFT_WHITE,TFT_BLACK);

  ledcAttach(PIN_LCD_BL, 2000, 8);
  ledcWrite(PIN_LCD_BL, 255);
  BOOTLOG_LN("[BOOT] backlight on");

  // APA102 LED ring
  FastLED.addLeds<APA102, APA102_DATA_PIN, APA102_CLK_PIN, BGR>(leds, APA102_NUM_LEDS);
  FastLED.setBrightness(22);
  FastLED.clear(true);
  BOOTLOG_LN("[BOOT] APA102 ring init done");

  lastInteraction = millis();

/*  // Splash - Remove or change it for your introduction text.
  display.clearDisplay();
  print(0, 0, NULL, 2, "PU2CLR");
  print(0, 15, NULL, 2, "ESP32");
  display.display();
  delay(2000);
  display.clearDisplay();
  print(0, 0, NULL, 2, "SI473X");
  print(0, 15, NULL, 2, "Arduino");
  display.display();
  // End Splash

  delay(2000);
  display.clearDisplay();
*/

  EEPROM.begin(EEPROM_SIZE);
  BOOTLOG_LN("[BOOT] EEPROM begin done");

  // The stored-settings wipe used to live here as "hold the encoder while powering
  // on". That gesture is both unreachable and dangerous on this board -- see the
  // note above the "Reset Cfg" menu entry -- so it now lives in the menu instead.

  // ICACHE_RAM_ATTR void rotaryEncoder(); see rotaryEncoder implementation below.
  attachInterrupt(digitalPinToInterrupt(ENCODER_PIN_A), rotaryEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_PIN_B), rotaryEncoder, CHANGE);
  BOOTLOG_LN("[BOOT] encoder ISRs attached");

  rx.setI2CFastModeCustom(100000);

  BOOTLOG_LN("[BOOT] probing Si4735 I2C ...");
  int16_t si4735Addr = rx.getDeviceI2CAddress(RESET_PIN); // Looks for the I2C bus address and set it.  Returns 0 if error
  BOOTLOG("[BOOT] Si4735 I2C addr=0x%02X\n", si4735Addr);

  if ( si4735Addr == 0 ) {
    tft.setTextSize(2);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.println("Si4735 not detected");
    // Unconditional: this is the one message a bring-up log exists for, and
    // honouring the CAT mute here produced a board that enumerated a USB port,
    // printed nothing and answered nothing, with the only off switch behind a
    // menu that boot never reaches.
    Serial.println("[BOOT] Si4735 NOT detected, halting");

    // Bring the transports up here. catStartTask() normally runs at the end of
    // setup(), which this halt never reaches -- so the old loop polled nothing and
    // answered nothing, the exact opposite of what it promised. netPoll() has to run
    // too, or the station never finishes associating and the TCP transport is dead.
    radioPresent = false;
    netBegin();
    catStartTask();
    while (1) { netPoll(); catServiceFromLoop(); delay(50); }   // ZZB;/ID; stay answerable
  }
  
  rx.setup(RESET_PIN, MW_BAND_TYPE);
  // Comment the line above and uncomment the three lines below if you are using external ref clock (active crystal or signal generator)
  // rx.setRefClock(32768);
  // rx.setRefClockPrescaler(1);   // will work with 32768  
  // rx.setup(RESET_PIN, 0, MW_BAND_TYPE, SI473X_ANALOG_AUDIO, XOSCEN_RCLK);

  rx.setAudioMuteMcuPin(AUDIO_MUTE);  
  
  cleanBfoRdsInfo();
  
  delay(300);


  // Checking the EEPROM content
  if (EEPROM.read(eeprom_address) == app_id)
  {
    readAllReceiverInformation();
  } else 
    rx.setVolume(volume);

  useBand();

  // Brings the station up (or raises the setup portal) only if the WiFi CAT
  // transport was left enabled. Entirely non-blocking: netPoll() drives it.
  netBegin();

  // Start the CAT service task now that the receiver is initialised: it answers
  // from the snapshot catServiceFromLoop() refreshes, so it must never run first.
  catStartTask();

  // Re-assert the button pull-up now that every library has had its turn at the
  // GPIO matrix. This is the configuration that measurably reads correctly.
  releaseEncoderButtonPin();
  pinMode(ENCODER_PUSH_BUTTON, INPUT_PULLUP);
  delay(20);
  btnSeenReleased = (digitalRead(ENCODER_PUSH_BUTTON) == HIGH);
  btnFaulty = false;
  btnLowSince = 0;

  showStatus();
  drawSprite();
}


/**
 * Prints a given content on display 
 */
void print(uint8_t col, uint8_t lin, const GFXfont *font, uint8_t textSize, const char *msg) {
  tft.setCursor(col,lin);
  tft.setTextSize(textSize);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.println(msg);
}

void printParam(const char *msg) {
 tft.fillScreen(TFT_BLACK);
 print(0,10,NULL,2, msg);
 }

/*
   writes the conrrent receiver information into the eeprom.
   The EEPROM.update avoid write the same data in the same memory position. It will save unnecessary recording.
*/
void saveAllReceiverInformation()
{
  int addr_offset;

  EEPROM.begin(EEPROM_SIZE);

  EEPROM.write(eeprom_address, app_id);                 // stores the app id;
  EEPROM.write(eeprom_address + 1, rx.getVolume()); // stores the current Volume
  EEPROM.write(eeprom_address + 2, bandIdx);            // Stores the current band
  EEPROM.write(eeprom_address + 3, fmRDS);
  EEPROM.write(eeprom_address + 4, currentMode); // Stores the current Mode (FM / AM / SSB)
  EEPROM.write(eeprom_address + 5, currentBFO >> 8);
  EEPROM.write(eeprom_address + 6, currentBFO & 0XFF);
  EEPROM.commit();

  addr_offset = 7;
  band[bandIdx].currentFreq = currentFrequency;

  for (int i = 0; i <= lastBand; i++)
  {
    EEPROM.write(addr_offset++, (band[i].currentFreq >> 8));   // stores the current Frequency HIGH byte for the band
    EEPROM.write(addr_offset++, (band[i].currentFreq & 0xFF)); // stores the current Frequency LOW byte for the band
    EEPROM.write(addr_offset++, band[i].currentStepIdx);       // Stores current step of the band
    EEPROM.write(addr_offset++, band[i].bandwidthIdx);         // table index (direct position) of bandwidth
  }

  // One commit, not one per band. EEPROM is NVS-backed here, so each commit
  // rewrote the whole 512-byte blob: 21 flash transactions per save, which stalled
  // the flash cache long enough to delay a CAT reply by over a second.
  EEPROM.commit();
  EEPROM.end();
}

/**
 * reads the last receiver status from eeprom. 
 */
void readAllReceiverInformation()
{
  uint8_t volume;
  int addr_offset;
  int bwIdx;
  EEPROM.begin(EEPROM_SIZE);

  volume = EEPROM.read(eeprom_address + 1); // Gets the stored volume;
  bandIdx = EEPROM.read(eeprom_address + 2);
  fmRDS = EEPROM.read(eeprom_address + 3);
  currentMode = EEPROM.read(eeprom_address + 4);
  currentBFO = EEPROM.read(eeprom_address + 5) << 8;
  currentBFO |= EEPROM.read(eeprom_address + 6);

  addr_offset = 7;
  for (int i = 0; i <= lastBand; i++)
  {
    band[i].currentFreq = EEPROM.read(addr_offset++) << 8;
    band[i].currentFreq |= EEPROM.read(addr_offset++);
    band[i].currentStepIdx = EEPROM.read(addr_offset++);
    band[i].bandwidthIdx = EEPROM.read(addr_offset++);
  }

  EEPROM.end();

  // Nothing above is trustworthy: the only gate is app_id, which says nothing
  // about the size of the band table. Editing that table (documented right above
  // it) used to be recovered with the power-on wipe gesture, which is gone now
  // because GPIO0 made it unreachable and dangerous -- so validate here instead.
  // An out-of-range bandIdx indexed band[] out of bounds and drawSprite()
  // dereferenced the garbage bandName, i.e. a boot loop with no way back in.
  if (bandIdx < 0 || bandIdx > lastBand) bandIdx = 0;
  if (currentMode > AM) currentMode = FM;
  for (int i = 0; i <= lastBand; i++)
  {
    int8_t lastStep = (band[i].bandType == FM_BAND_TYPE) ? lastFmStep : lastAmStep;
    if (band[i].currentStepIdx < 0 || band[i].currentStepIdx > lastStep)
      band[i].currentStepIdx = 0;
    if (band[i].bandwidthIdx < 0 || band[i].bandwidthIdx > lastBandwidthAM)
      band[i].bandwidthIdx = 0;
    if (band[i].currentFreq < band[i].minimumFreq ||
        band[i].currentFreq > band[i].maximumFreq)
      band[i].currentFreq = band[i].minimumFreq;
  }
  if (volume > 63) volume = DEFAULT_VOLUME;

  currentFrequency = band[bandIdx].currentFreq;

  if (band[bandIdx].bandType == FM_BAND_TYPE)
  {
    currentStepIdx = idxFmStep = band[bandIdx].currentStepIdx;
    rx.setFrequencyStep(tabFmStep[currentStepIdx]);
  }
  else
  {
    currentStepIdx = idxAmStep = band[bandIdx].currentStepIdx;
    rx.setFrequencyStep(tabAmStep[currentStepIdx]);
  }

  bwIdx = band[bandIdx].bandwidthIdx;

  if (currentMode == LSB || currentMode == USB)
  {
    loadSSB();
    bwIdxSSB = (bwIdx > 5) ? 5 : bwIdx;
    rx.setSSBAudioBandwidth(bandwidthSSB[bwIdxSSB].idx);
    // If audio bandwidth selected is about 2 kHz or below, it is recommended to set Sideband Cutoff Filter to 0.
    if (bandwidthSSB[bwIdxSSB].idx == 0 || bandwidthSSB[bwIdxSSB].idx == 4 || bandwidthSSB[bwIdxSSB].idx == 5)
      rx.setSSBSidebandCutoffFilter(0);
    else
      rx.setSSBSidebandCutoffFilter(1);
    rx.setSSBBfo(currentBFO);      
  }
  else if (currentMode == AM)
  {
    bwIdxAM = bwIdx;
    rx.setBandwidth(bandwidthAM[bwIdxAM].idx, 1);
  }
  else
  {
    bwIdxFM = bwIdx;
    rx.setFmBandwidth(bandwidthFM[bwIdxFM].idx);
  }

  if (currentBFO > 0)
    sprintf(bfo, "+%4.4d", currentBFO);
  else
    sprintf(bfo, "%4.4d", currentBFO);

  delay(50);
  rx.setVolume(volume);
}

/*
 * To store any change into the EEPROM, it is needed at least STORE_TIME  milliseconds of inactivity.
 */
void resetEepromDelay()
{
  elapsedCommand = storeTime = millis();
  itIsTimeToSave = true;
}

// The CAT equivalent. resetEepromDelay() also pushes elapsedCommand, which is the
// anchor for the on-screen menu teardown -- so remote activity held whatever menu
// the user had open. It also postponed storeTime, so a client setting a frequency
// more often than STORE_TIME meant nothing was ever written to EEPROM. Mark it
// dirty and let the existing deadline run out on its own.
static void catMarkDirty()
{
  if (!itIsTimeToSave)
  {
    itIsTimeToSave = true;
    storeTime = millis();
  }
}

/**
    Set all command flags to false
    When all flags are disabled (false), the encoder controls the frequency
*/
void disableCommands()
{
  cmdConfirmReset = false;   // never leave a destructive prompt armed
  cmdBand = false;
  bfoOn = false;
  cmdVolume = false;
  cmdAgc = false;
  cmdBandwidth = false;
  cmdStep = false;
  cmdMode = false;
  cmdMenu = false;
  cmdSoftMuteMaxAtt = false;
  countClick = 0;
  // showCommandStatus((char *) "VFO ");
}

/**
 * Reads encoder via interrupt
 * Use Rotary.h and  Rotary.cpp implementation to process encoder via interrupt
 * if you do not add ICACHE_RAM_ATTR declaration, the system will reboot during attachInterrupt call. 
 * With ICACHE_RAM_ATTR macro you put the function on the RAM.
 */
ICACHE_RAM_ATTR void  rotaryEncoder()
{ // rotary encoder events
  uint8_t encoderStatus = encoder.process();
  if (encoderStatus)
  {
    // Accumulate. Latching a single +/-1 threw away every detent that arrived
    // while loop() was busy -- a 108 KB sprite push, or the setup portal's
    // blocking handleClient() -- so fast spins under load simply lost counts.
    int c = encoderCount + ((encoderStatus == DIR_CW) ? -1 : 1); // CCW increments
    if (c > 64) c = 64;
    if (c < -64) c = -64;
    encoderCount = c;
  }
}

/**
 * Shows frequency information on Display
 */
void showFrequency()
{
  char tmp[15];
  sprintf(tmp, "%5.5u", currentFrequency);
  drawSprite();
  // showMode();
}

/**
 * Shows the current mode
 */
void showMode() {
  drawSprite();    
}

/**
 * Shows some basic information on display
 */
void showStatus()
{
  showFrequency();
  showRSSI();
}

/**
 *  Shows the current Bandwidth status
 */
void showBandwidth()
{
  drawSprite();
}

/**
 *   Shows the current RSSI and SNR status
 */
void showRSSI()
{
  char sMeter[10];
  sprintf(sMeter, "S:%d ", rssi);
  drawSprite();
}

/**
 *    Shows the current AGC and Attenuation status
 */
void showAgcAtt()
{
  // lcd.clear();
  rx.getAutomaticGainControl();
  if (agcNdx == 0 && agcIdx == 0)
    strcpy(sAgc, "AGC ON");
  else
    sprintf(sAgc, "ATT: %2.2d", agcNdx);

  drawSprite();

}

/**
 *   Shows the current step
 */
void showStep()
{
  drawSprite();
}

/**
 *  Shows the current BFO value
 */
void showBFO()
{
  
  if (currentBFO > 0)
    sprintf(bfo, "+%4.4d", currentBFO);
  else
    sprintf(bfo, "%4.4d", currentBFO);
  drawSprite();
  elapsedCommand = millis();
}

/*
 *  Shows the volume level on LCD
 */
// The receiver's own volume is 0..63, Kenwood CAT carries 0..255, and PC software
// renders that as 0..100 -- so the same setting read 11 here and 17 in Flrig. Show
// the percentage instead so both ends agree. Truncating (rather than rounding)
// matches how Flrig scales the CAT value back down, to within a step.
static uint8_t volumePercent()
{
  return (uint8_t)(((uint16_t)rx.getVolume() * 100u) / 63u);
}

void showVolume()
{
drawSprite();
}

/**
 * Show Soft Mute 
 */
void showSoftMute()
{
  drawSprite();
}

/**
 *   Sets Band up (1) or down (!1)
 */
void setBand(int8_t up_down)
{
  band[bandIdx].currentFreq = currentFrequency;
  band[bandIdx].currentStepIdx = currentStepIdx;
  if (up_down == -1)
    bandIdx = (bandIdx < lastBand) ? (bandIdx + 1) : 0;
  else
    bandIdx = (bandIdx > 0) ? (bandIdx - 1) : lastBand;
  useBand();
  delay(MIN_ELAPSED_TIME); // waits a little more for releasing the button.
  elapsedCommand = millis();
}

/**
 * Switch the radio to current band
 */
void useBand()
{
  if (band[bandIdx].bandType == FM_BAND_TYPE)
  {
    currentMode = FM;
    rx.setTuneFrequencyAntennaCapacitor(0);
    rx.setFM(band[bandIdx].minimumFreq, band[bandIdx].maximumFreq, band[bandIdx].currentFreq, tabFmStep[band[bandIdx].currentStepIdx]);
    rx.setSeekFmLimits(band[bandIdx].minimumFreq, band[bandIdx].maximumFreq);
    bfoOn = ssbLoaded = false;
    bwIdxFM = band[bandIdx].bandwidthIdx;
    rx.setFmBandwidth(bandwidthFM[bwIdxFM].idx);
    rx.setFMDeEmphasis(1);
    rx.RdsInit();
    rx.setRdsConfig(1, 2, 2, 2, 2);
  }
  else
  {
    // set the tuning capacitor for SW or MW/LW
    rx.setTuneFrequencyAntennaCapacitor((band[bandIdx].bandType == MW_BAND_TYPE || band[bandIdx].bandType == LW_BAND_TYPE) ? 0 : 1);
    if (ssbLoaded)
    {
      rx.setSSB(band[bandIdx].minimumFreq, band[bandIdx].maximumFreq, band[bandIdx].currentFreq, tabAmStep[band[bandIdx].currentStepIdx], currentMode);
      rx.setSSBAutomaticVolumeControl(1);
      rx.setSsbSoftMuteMaxAttenuation(softMuteMaxAttIdx); // Disable Soft Mute for SSB
      // bandwidthAM[] has 7 entries but bandwidthSSB[] only 6, and a band that was
      // last used in AM can carry index 6. Unclamped, the SSB path read one struct
      // past the array and drawSprite() dereferenced its .desc as a char*. The
      // EEPROM loader already clamps this; useBand() did not, and CAT's MD1; now
      // reaches it remotely.
      bwIdxSSB = (band[bandIdx].bandwidthIdx > lastBandwidthSSB)
                   ? lastBandwidthSSB : band[bandIdx].bandwidthIdx;
      rx.setSSBAudioBandwidth(bandwidthSSB[bwIdxSSB].idx);
    }
    else
    {
      currentMode = AM;
      rx.setAM(band[bandIdx].minimumFreq, band[bandIdx].maximumFreq, band[bandIdx].currentFreq, tabAmStep[band[bandIdx].currentStepIdx]);
      bfoOn = false;
      bwIdxAM = band[bandIdx].bandwidthIdx;
      rx.setBandwidth(bandwidthAM[bwIdxAM].idx, 1);
      rx.setAmSoftMuteMaxAttenuation(softMuteMaxAttIdx); // Soft Mute for AM or SSB
    }
    rx.setAutomaticGainControl(disableAgc, agcNdx);
    rx.setSeekAmLimits(band[bandIdx].minimumFreq, band[bandIdx].maximumFreq); // Consider the range all defined current band
    rx.setSeekAmSpacing(5); // Max 10kHz for spacing

  }
  delay(100);
  if (band[bandIdx].bandType != FM_BAND_TYPE) lastAmBandIdx = bandIdx;
  currentFrequency = band[bandIdx].currentFreq;
  currentStepIdx = band[bandIdx].currentStepIdx;

  rssi = 0;
  snr = 0;

  // setFM/setAM/setSSB above re-enable the audio, but `muted` still says muted,
  // so the UI and CAT both claimed mute while sound was coming out. Re-apply it.
  if (muted) rx.setAudioMute(true);

  cleanBfoRdsInfo();
  showStatus();
}


void loadSSB() {
  rx.setI2CFastModeCustom(400000); // You can try rx.setI2CFastModeCustom(700000); or greater value
  rx.loadPatch(ssb_patch_content, size_content, bandwidthSSB[bwIdxSSB].idx);
  rx.setI2CFastModeCustom(100000);
  ssbLoaded = true; 
}

/**
 *  Switches the Bandwidth
 */
void doBandwidth(int8_t v)
{
    if (currentMode == LSB || currentMode == USB)
    {
      bwIdxSSB = (v == 1) ? bwIdxSSB + 1 : bwIdxSSB - 1;

      if (bwIdxSSB > maxSsbBw)
        bwIdxSSB = 0;
      else if (bwIdxSSB < 0)
        bwIdxSSB = maxSsbBw;

      rx.setSSBAudioBandwidth(bandwidthSSB[bwIdxSSB].idx);
      // If audio bandwidth selected is about 2 kHz or below, it is recommended to set Sideband Cutoff Filter to 0.
      if (bandwidthSSB[bwIdxSSB].idx == 0 || bandwidthSSB[bwIdxSSB].idx == 4 || bandwidthSSB[bwIdxSSB].idx == 5)
        rx.setSSBSidebandCutoffFilter(0);
      else
        rx.setSSBSidebandCutoffFilter(1);

      band[bandIdx].bandwidthIdx = bwIdxSSB;
    }
    else if (currentMode == AM)
    {
      bwIdxAM = (v == 1) ? bwIdxAM + 1 : bwIdxAM - 1;

      if (bwIdxAM > maxAmBw)
        bwIdxAM = 0;
      else if (bwIdxAM < 0)
        bwIdxAM = maxAmBw;

      rx.setBandwidth(bandwidthAM[bwIdxAM].idx, 1);
      band[bandIdx].bandwidthIdx = bwIdxAM;
      
    } else {
    bwIdxFM = (v == 1) ? bwIdxFM + 1 : bwIdxFM - 1;
    if (bwIdxFM > maxFmBw)
      bwIdxFM = 0;
    else if (bwIdxFM < 0)
      bwIdxFM = maxFmBw;

    rx.setFmBandwidth(bandwidthFM[bwIdxFM].idx);
    band[bandIdx].bandwidthIdx = bwIdxFM;
  }
  showBandwidth();
  delay(MIN_ELAPSED_TIME); // waits a little more for releasing the button.
}

/**
 * Show cmd on display. It means you are setting up something.  
 */
void showCommandStatus(char * currentCmd)
{
  spr.drawString(currentCmd,38,14,2);
  drawSprite();
}

/**
 * Show menu options
 */
void showMenu() {
  drawSprite();
}

/**
 *  AGC and attenuattion setup
 */
void doAgc(int8_t v) {
  agcIdx = (v == 1) ? agcIdx + 1 : agcIdx - 1;
  if (agcIdx < 0 )
    agcIdx = 35;
  else if ( agcIdx > 35)
    agcIdx = 0;
  disableAgc = (agcIdx > 0); // if true, disable AGC; esle, AGC is enable
  if (agcIdx > 1)
    agcNdx = agcIdx - 1;
  else
    agcNdx = 0;
  rx.setAutomaticGainControl(disableAgc, agcNdx); // if agcNdx = 0, no attenuation
  showAgcAtt();
  delay(MIN_ELAPSED_TIME); // waits a little more for releasing the button.
  elapsedCommand = millis();
}


/**
 * Switches the current step
 */
void doStep(int8_t v)
{
    if ( currentMode == FM ) {
      idxFmStep = (v == 1) ? idxFmStep + 1 : idxFmStep - 1;
      if (idxFmStep > lastFmStep)
        idxFmStep = 0;
      else if (idxFmStep < 0)
        idxFmStep = lastFmStep;
        
      currentStepIdx = idxFmStep;
      rx.setFrequencyStep(tabFmStep[currentStepIdx]);
      
    } else {
      idxAmStep = (v == 1) ? idxAmStep + 1 : idxAmStep - 1;
      if (idxAmStep > lastAmStep)
        idxAmStep = 0;
      else if (idxAmStep < 0)
        idxAmStep = lastAmStep;

      currentStepIdx = idxAmStep;
      rx.setFrequencyStep(tabAmStep[currentStepIdx]);
      rx.setSeekAmSpacing(5); // Max 10kHz for spacing
    }
    band[bandIdx].currentStepIdx = currentStepIdx;
    showStep();
    elapsedCommand = millis();
}

/**
 * Switches to the AM, LSB or USB modes
 */
void doMode(int8_t v)
{
  if (currentMode != FM)
  {
    if (v == 1)  { // clockwise
      if (currentMode == AM)
      {
        // If you were in AM mode, it is necessary to load SSB patch (avery time)

        spr.fillSmoothRoundRect(80,40,160,40,4,TFT_WHITE);
        spr.fillSmoothRoundRect(81,41,158,38,4,TFT_MENU_BACK);
        spr.drawString("Loading SSB",160,62,4);
        spr.pushSprite(0,0);
        
        loadSSB();
        ssbLoaded = true;
        currentMode = LSB;
      }
      else if (currentMode == LSB)
        currentMode = USB;
      else if (currentMode == USB)
      {
        currentMode = AM;
        bfoOn = ssbLoaded = false;
      }
    } else { // and counterclockwise
      if (currentMode == AM)
      {
        // If you were in AM mode, it is necessary to load SSB patch (avery time)

        spr.fillSmoothRoundRect(80,40,160,40,4,TFT_WHITE);
        spr.fillSmoothRoundRect(81,41,158,38,4,TFT_MENU_BACK);
        spr.drawString("Loading SSB",160,62,4);
        spr.pushSprite(0,0);
        
        loadSSB();
        ssbLoaded = true;
        currentMode = USB;
      }
      else if (currentMode == USB)
        currentMode = LSB;
      else if (currentMode == LSB)
      {
        currentMode = AM;
        bfoOn = ssbLoaded = false;
      }
    }
    // Nothing to do if you are in FM mode
    band[bandIdx].currentFreq = currentFrequency;
    band[bandIdx].currentStepIdx = currentStepIdx;
    useBand();
  }
  delay(MIN_ELAPSED_TIME); // waits a little more for releasing the button.
  elapsedCommand = millis();
}

/**
 * Sets the audio volume
 */
void doVolume( int8_t v ) {
  if ( v == 1)
    rx.volumeUp();
  else
    rx.volumeDown();

  showVolume();
  delay(MIN_ELAPSED_TIME); // waits a little more for releasing the button.
}

/**
 *  This function is called by the seek function process.
 */
void showFrequencySeek(uint16_t freq)
{
  currentFrequency = freq;
  showFrequency();
}

/**
 *  Find a station. The direction is based on the last encoder move clockwise or counterclockwise
 */
void doSeek()
{
  if ((currentMode == LSB || currentMode == USB)) return; // It does not work for SSB mode
  
  rx.seekStationProgress(showFrequencySeek, seekDirection);
  currentFrequency = rx.getFrequency();
  
}

/**
 * Sets the Soft Mute Parameter
 */
void doSoftMute(int8_t v)
{
  softMuteMaxAttIdx = (v == 1) ? softMuteMaxAttIdx + 1 : softMuteMaxAttIdx - 1;
  if (softMuteMaxAttIdx > 32)
    softMuteMaxAttIdx = 0;
  else if (softMuteMaxAttIdx < 0)
    softMuteMaxAttIdx = 32;

  rx.setAmSoftMuteMaxAttenuation(softMuteMaxAttIdx);
  showSoftMute();
  elapsedCommand = millis();
}

/**
 *  Menu options selection
 */
void doMenu( int8_t v) {
  menuIdx = (v == 1) ? menuIdx - 1 : menuIdx + 1;

  if (menuIdx > lastMenu)
    menuIdx = 0;
  else if (menuIdx < 0)
    menuIdx = lastMenu;

  showMenu();
  delay(MIN_ELAPSED_TIME); // waits a little more for releasing the button.
  elapsedCommand = millis();
}


/**
 * Starts the MENU action process
 */
// Clears the stored receiver settings and the CAT/WiFi preference namespaces, then
// restarts. Wiping only the EEPROM byte left the CAT and WiFi settings behind, so
// a "reset" could not recover from a bad CAT port or a broken saved network.
static void performConfigReset()
{
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(eeprom_address, 0);
  EEPROM.commit();
  EEPROM.end();

  Preferences p;
  if (p.begin("cat", false))     { p.clear(); p.end(); }
  if (p.begin("wifinet", false)) { p.clear(); p.end(); }

  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(20, 70);
  tft.println("Settings erased");
  delay(1500);
  ESP.restart();
}

void doCurrentMenuCmd() {
  disableCommands();

  switch (currentMenuCmd) {
     case VOLUME:                 // VOLUME
      cmdVolume = true;
      showVolume();
      break;
    case STEP:                 // STEP
      cmdStep = true;
      showStep();
      break;
    case MODE:                 // MODE
      cmdMode = true;
      showMode();
      break;
    case BFO:
      if ((currentMode == LSB || currentMode == USB)) {
        bfoOn = true;
        showBFO();
      }
      showFrequency();
      break;      
    case BW:                 // BW
      cmdBandwidth = true;
      showBandwidth();
      break;
    case AGC_ATT:                 // AGC/ATT
      cmdAgc = true;
      showAgcAtt();
      break;
    case SOFTMUTE: 
      cmdSoftMuteMaxAtt = true;
      showSoftMute();  
      break;
    case SEEKUP:
      seekDirection = 1;
      doSeek();
      break;  
    case SEEKDOWN:
      seekDirection = 0;
      doSeek();
      break;    
    case BAND: 
      cmdBand = true;
      drawSprite();  
      break;
    case MUTE: 
      muted=!muted;
      if (muted) rx.setAudioMute(muted);
      else rx.setAudioMute(muted);
      drawSprite();  
      break;
    case CAT_USB:
      // Takes over the USB CDC port; the boot log goes quiet while it is on.
      catUsbSetEnabled(!catUsbGetEnabled());
      showToast(catUsbGetEnabled() ? "CAT USB  ON" : "CAT USB  OFF");
      drawSprite();
      break;
    case CAT_WIFI:
      // Enabling with no saved network raises the setup portal by itself.
      netSetEnabled(!netEnabled());
      if (!netEnabled())          showToast("CAT WiFi  OFF");
      else if (netSuspended())    showToast("CAT WiFi: USB attached");
      else                        showToast("CAT WiFi  ON");
      drawSprite();
      break;
    case RESET_CFG:
      // Opens the confirmation modal; nothing is erased here. This replaces the
      // old power-on gesture, which fired on a deep-sleep wake because GPIO0 --
      // the encoder button -- is also the ext0 wake source, so switching the radio
      // on by holding the button erased every stored setting.
      cmdConfirmReset = true;
      confirmEraseSel = false;          // Cancel is the default
      drawSprite();
      break;
    case WIFI_CFG:
      // Opens the setup portal without switching the transport on: closing it puts
      // the WiFi hardware back to sleep unless a network was actually saved.
      if (netPortalActive()) { netStopPortal(); showToast("WiFi setup closed"); }
      else                   { netOpenSetup();  showToast("WiFi setup portal"); }
      drawSprite();
      break;
    default:
      showStatus();
      break;
  }
  currentMenuCmd = -1;
  elapsedCommand = millis();
}

/**
 * Return true if the current status is Menu command
 */
bool isMenuMode() {
  return (cmdMenu | cmdStep | cmdBandwidth | cmdAgc | cmdVolume | cmdSoftMuteMaxAtt | cmdMode);
}

uint8_t getStrength() {
  if (currentMode != FM) {
    //dBuV to S point conversion HF
    if ((rssi >= 0) and (rssi <=  1)) return  1;  // S0
    if ((rssi >  1) and (rssi <=  1)) return  2;  // S1
    if ((rssi >  2) and (rssi <=  3)) return  3;  // S2
    if ((rssi >  3) and (rssi <=  4)) return  4;  // S3
    if ((rssi >  4) and (rssi <= 10)) return  5;  // S4
    if ((rssi > 10) and (rssi <= 16)) return  6;  // S5
    if ((rssi > 16) and (rssi <= 22)) return  7;  // S6
    if ((rssi > 22) and (rssi <= 28)) return  8;  // S7
    if ((rssi > 28) and (rssi <= 34)) return  9;  // S8
    if ((rssi > 34) and (rssi <= 44)) return 10;  // S9
    if ((rssi > 44) and (rssi <= 54)) return 11;  // S9 +10
    if ((rssi > 54) and (rssi <= 64)) return 12;  // S9 +20
    if ((rssi > 64) and (rssi <= 74)) return 13;  // S9 +30
    if ((rssi > 74) and (rssi <= 84)) return 14;  // S9 +40
    if ((rssi > 84) and (rssi <= 94)) return 15;  // S9 +50
    if  (rssi > 94)                   return 16;  // S9 +60
    if  (rssi > 95)                   return 17;  //>S9 +60
  }
  else
  {
    //dBuV to S point conversion FM
    if  (rssi <  1)                   return  1;
    if ((rssi >  1) and (rssi <=  2)) return  7;  // S6
    if ((rssi >  2) and (rssi <=  8)) return  8;  // S7
    if ((rssi >  8) and (rssi <= 14)) return  9;  // S8
    if ((rssi > 14) and (rssi <= 24)) return 10;  // S9
    if ((rssi > 24) and (rssi <= 34)) return 11;  // S9 +10
    if ((rssi > 34) and (rssi <= 44)) return 12;  // S9 +20
    if ((rssi > 44) and (rssi <= 54)) return 13;  // S9 +30
    if ((rssi > 54) and (rssi <= 64)) return 14;  // S9 +40
    if ((rssi > 64) and (rssi <= 74)) return 15;  // S9 +50
    if  (rssi > 74)                   return 16;  // S9 +60
    if  (rssi > 76)                   return 17;  //>S9 +60
    // newStereoPilot=si4735.getCurrentPilot();
  }
  return 0;
}    

void drawMenu() {
  if (cmdMenu) {
    spr.fillSmoothRoundRect(1,1,76,110,4,TFT_RED);
    spr.fillSmoothRoundRect(2,2,74,108,4,TFT_MENU_BACK);
    spr.setTextColor(TFT_WHITE,TFT_MENU_BACK);    
    spr.drawString("Menu",38,14,2);
    spr.setTextFont(0);
    spr.setTextColor(0xBEDF,TFT_MENU_BACK);
    spr.fillRoundRect(6,24+(2*16),66,16,2,0x105B);
    for(int i=-2;i<3;i++){
      if (i==0) spr.setTextColor(0xBEDF,0x105B);
      else spr.setTextColor(0xBEDF,TFT_MENU_BACK);
      spr.drawString(menu[abs((menuIdx+lastMenu+1+i)%(lastMenu+1))],38,64+(i*16),2);
    }
  } else {
    spr.setTextColor(TFT_WHITE,TFT_MENU_BACK);    
    spr.fillSmoothRoundRect(1,1,76,110,4,TFT_RED);
    spr.fillSmoothRoundRect(2,2,74,108,4,TFT_MENU_BACK);
    spr.drawString(menu[menuIdx],38,14,2);
    spr.setTextFont(0);
    spr.setTextColor(0xBEDF,TFT_MENU_BACK);
    // spr.fillRect(6,24+(2*16),67,16,0xBEDF);
    spr.fillRoundRect(6,24+(2*16),66,16,2,0x105B);
    for(int i=-2;i<3;i++){
      if (i==0) spr.setTextColor(0xBEDF,0x105B);
      else spr.setTextColor(0xBEDF,TFT_MENU_BACK);
      if (cmdMode)
        if (currentMode == FM) {
          if (i==0) spr.drawString(bandModeDesc[abs((currentMode+lastBandModeDesc+1+i)%(lastBandModeDesc+1))],38,64+(i*16),2);
        }          
        else spr.drawString(bandModeDesc[abs((currentMode+lastBandModeDesc+1+i)%(lastBandModeDesc+1))],38,64+(i*16),2);
      if (cmdStep)
        if (currentMode == FM) spr.drawNumber(tabFmStep[abs((currentStepIdx+lastFmStep+1+i)%(lastFmStep+1))],38,64+(i*16),2);
        else spr.drawNumber(tabAmStep[abs((currentStepIdx+lastAmStep+1+i)%(lastAmStep+1))],38,64+(i*16),2);
      if (cmdBand) spr.drawString(band[abs((bandIdx+lastBand+1+i)%(lastBand+1))].bandName,38,64+(i*16),2);
      if (cmdBandwidth) {
        if (currentMode == LSB || currentMode == USB)
        {
          spr.drawString(bandwidthSSB[abs((bwIdxSSB+lastBandwidthSSB+1+i)%(lastBandwidthSSB+1))].desc,38,64+(i*16),2);
          // bw = (char *)bandwidthSSB[bwIdxSSB].desc;
          // showBFO();
        }
        else if (currentMode == AM)
        {
          spr.drawString(bandwidthAM[abs((bwIdxAM+lastBandwidthAM+1+i)%(lastBandwidthAM+1))].desc,38,64+(i*16),2);
        }
        else
        {
          spr.drawString(bandwidthFM[abs((bwIdxFM+lastBandwidthFM+1+i)%(lastBandwidthFM+1))].desc,38,64+(i*16),2);
        }
      }
    }
    if (cmdVolume) {
      spr.setTextColor(0xBEDF,TFT_MENU_BACK);
      spr.fillRoundRect(6,24+(2*16),66,16,2,TFT_MENU_BACK);
      spr.drawNumber(volumePercent(),38,60,7);
    }
    if (cmdAgc) {
      spr.setTextColor(0xBEDF,TFT_MENU_BACK);
      spr.fillRoundRect(6,24+(2*16),66,16,2,TFT_MENU_BACK);
      rx.getAutomaticGainControl();
      if (agcNdx == 0 && agcIdx == 0) {
        spr.setFreeFont(&Orbitron_Light_24);
        spr.drawString("AGC",38,48);
        spr.drawString("On",38,72);
        spr.setTextFont(0);
      } else {
        sprintf(sAgc, "%2.2d", agcNdx);
        spr.drawString(sAgc,38,60,7);
      }
    }        
    if (cmdSoftMuteMaxAtt) {
      spr.setTextColor(0xBEDF,TFT_MENU_BACK);
      spr.fillRoundRect(6,24+(2*16),66,16,2,TFT_MENU_BACK);
      spr.drawNumber(softMuteMaxAttIdx,38,60,7);
    }
    spr.setTextColor(TFT_WHITE,TFT_BLACK);
  }
}

#define ST_ON    TFT_GREEN
#define ST_WAIT   TFT_YELLOW
#define ST_AP     TFT_CYAN
#define ST_IDLE   0x8410

// A small labelled chip: outlined when the transport is enabled but nobody is
// talking to it, filled when a client is actually connected.
static void drawStatusChip(int x, int y, int w, const char *label, bool filled, uint16_t color)
{
  if (filled) {
    spr.fillRoundRect(x, y, w, 13, 3, color);
    spr.setTextColor(TFT_BLACK, color);
  } else {
    spr.drawRoundRect(x, y, w, 13, 3, color);
    spr.setTextColor(color, TFT_BLACK);
  }
  spr.setTextDatum(MC_DATUM);
  spr.drawString(label, x + w / 2, y + 6, 1);
}

// The USB trident, drawn small enough for the status strip: stem with an arrow, a
// round end, and the square/triangle branches. A lettered "U" chip was too easy to
// miss -- this reads as USB at a glance. Colour carries the state, as with WiFi.
static void drawUsbGlyph(int cx, int cy, uint16_t color)
{
  spr.drawLine(cx, cy - 6, cx, cy + 5, color);              // stem
  spr.fillTriangle(cx - 2, cy - 4, cx + 2, cy - 4, cx, cy - 8, color); // arrow head
  spr.fillCircle(cx, cy + 6, 2, color);                     // round end
  spr.drawLine(cx, cy + 1, cx - 4, cy + 1, color);          // left branch
  spr.drawLine(cx - 4, cy + 1, cx - 4, cy - 2, color);
  spr.fillRect(cx - 6, cy - 5, 4, 4, color);                // ...to a square
  spr.drawLine(cx, cy + 3, cx + 4, cy + 3, color);          // right branch
  spr.drawLine(cx + 4, cy + 3, cx + 4, cy, color);
  spr.fillTriangle(cx + 2, cy, cx + 6, cy, cx + 4, cy - 4, color); // ...to a triangle
}

// A short-lived confirmation banner. Toggling a transport from the menu changed only
// a small icon, which was easy to miss entirely, so say what happened in words.
static char     toastMsg[26] = "";
static uint32_t toastUntil = 0;

static void showToast(const char *msg)
{
  strlcpy(toastMsg, msg, sizeof(toastMsg));
  toastUntil = millis() + 1600;
  catUiDirty = true;
}

// Three-arc WiFi glyph: a dot with arcs above it. Colour carries the link state.
static void drawWifiGlyph(int cx, int cy, uint16_t color)
{
  spr.fillCircle(cx, cy, 1, color);
  spr.drawCircleHelper(cx, cy, 4, 3, color);   // 3 = both top corners = upper arc
  spr.drawCircleHelper(cx, cy, 7, 3, color);
}

// Status cluster in the free strip between the band name and the battery icon.
// Answers four things at a glance: is WiFi up, is the setup AP running, is a CAT
// client on the network, is a CAT client on USB. Laid out right to left so it
// stays tidy when only some of them are on.
static void drawStatusIcons()
{
  spr.setTextFont(0);
  int x = 286;                        // right edge, just left of the battery

  // Each glyph carries both facts in its colour: grey = the transport is on but
  // nobody is talking, green = a client is actively driving the radio.
  if (catUsbGetEnabled()) {
    x -= 15;
    drawUsbGlyph(x + 7, 9, catUsbClientActive() ? ST_ON : ST_IDLE);
    x -= 3;
  }

  if (netPortalActive()) {
    x -= 20;
    drawStatusChip(x, 3, 20, "AP", true, ST_AP);
    x -= 3;
  }

  // Driven by the hardware actually being powered, so nothing is drawn while WiFi
  // is off -- whether that is because it is disabled or suspended for USB.
  if (netRadioOn()) {
    uint16_t c;
    switch (netGetState()) {
      case NET_CONNECTED:  c = catNetClientActive() ? ST_ON : ST_IDLE; break;
      case NET_CONNECTING: c = ST_WAIT; break;
      case NET_PORTAL:     c = ST_AP;   break;
      default:             c = ST_IDLE; break;
    }
    x -= 16;
    drawWifiGlyph(x + 8, 10, c);
  }

  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  spr.setTextDatum(MC_DATUM);
}

void drawSprite()
{
  
  spr.fillSprite(TFT_BLACK);
  spr.setTextColor(TFT_WHITE,TFT_BLACK);

  if (currentMode == FM) spr.drawFloat(currentFrequency/100.00,1,160,60,7);
  else spr.drawNumber(currentFrequency,160,60,7);

  spr.setFreeFont(&Orbitron_Light_24);
  spr.drawString(band[bandIdx].bandName,160,12);
  
  if (isMenuMode() or cmdBand) drawMenu();    
  else {
    countClick = 0;
    spr.setTextDatum(ML_DATUM);
    spr.setTextColor(TFT_WHITE,TFT_MENU_BACK);    
    spr.fillSmoothRoundRect(1,1,76,110,4,TFT_WHITE);
    spr.fillSmoothRoundRect(2,2,74,108,4,TFT_MENU_BACK);
    spr.drawString("Band:",6,64+(-3*16),2);    
    spr.drawString(band[bandIdx].bandName,48,64+(-3*16),2);
    spr.drawString("Mode:",6,64+(-2*16),2);    
    spr.drawString(bandModeDesc[currentMode],48,64+(-2*16),2);    
    spr.drawString("Step:",6,64+(-1*16),2);    
    if (currentMode == FM) spr.drawNumber(tabFmStep[currentStepIdx],48,64+(-1*16),2);
    else spr.drawNumber(tabAmStep[currentStepIdx],48,64+(-1*16),2);
    spr.drawString("BW:",6,64+(0*16),2);        
    if (currentMode == LSB || currentMode == USB)
    {
      spr.drawString(bandwidthSSB[bwIdxSSB].desc,48,64+(0*16),2);
    }
    else if (currentMode == AM)
    {
      spr.drawString(bandwidthAM[bwIdxAM].desc,48,64+(0*16),2);
    }
    else
    {
      spr.drawString(bandwidthFM[bwIdxFM].desc,48,64+(0*16),2);
    }
    if (agcNdx == 0 && agcIdx == 0) {
      spr.drawString("AGC:",6,64+(1*16),2);        
      spr.drawString("On",48,64+(1*16),2);
    } else {
      sprintf(sAgc, "%2.2d", agcNdx);
      spr.drawString("Att:",6,64+(1*16),2);        
      spr.drawString(sAgc,48,64+(1*16),2);
    }
    spr.drawString("BFO:",6,64+(2*16),2);
    if (currentMode == LSB || currentMode == USB) {
      spr.setTextDatum(MR_DATUM);
      spr.drawString(bfo,74,64+(2*16),2);
    }
    else spr.drawString("Off",48,64+(2*16),2);
    spr.setTextDatum(MC_DATUM);
  }

  if (bfoOn) {
    spr.setTextColor(TFT_WHITE,TFT_BLACK);
    spr.drawString("BFO:",125,102,4);
    spr.setTextDatum(MR_DATUM);
    spr.drawString(bfo,225,102,4);
    spr.setTextDatum(MC_DATUM);    
  
  }

  spr.setTextFont(0);
  spr.setTextColor(TFT_WHITE,TFT_BLACK);
  
  spr.drawString("SIGNAL:",266,54);
  if (muted) {
      spr.setTextColor(TFT_RED,TFT_BLACK);
      spr.drawString("MUTE ON",272,102,2);
      spr.setTextColor(TFT_WHITE,TFT_BLACK);
  }
  else {
      spr.drawString("VOL:",257,102,2);    
      spr.drawNumber(volumePercent(),282,102,2);
  }
  
  for(int i=0;i<getStrength();i++)
    if (i<10)
      spr.fillRect(244+(i*4),80-(i*1),2,4+(i*1),0x3526);
    else
      spr.fillRect(244+(i*4),80-(i*1),2,4+(i*1),TFT_GREEN);


  spr.fillTriangle(156,112,160,122,164,112,TFT_GREEN);
  spr.drawLine(160,114,160,170,TFT_GREEN);

  int temp=(currentFrequency/10.00)-20;
  uint16_t lineColor;
  for(int i=0;i<40;i++)
  {
    if (i==20) lineColor=TFT_GREEN;
    else lineColor=0xC638;
    if (!(temp<band[bandIdx].minimumFreq/10.00 or temp>band[bandIdx].maximumFreq/10.00)) {
      if((temp%10)==0){
        spr.drawLine(i*8,170,i*8,140,lineColor);
        spr.drawLine((i*8)+1,170,(i*8)+1,140,lineColor);
        if (currentMode == FM) spr.drawFloat(temp/10.0,1,i*8,130,2);
        else if (temp >= 100) spr.drawFloat(temp/100.0,3,i*8,130,2);
               else spr.drawNumber(temp*10,i*8,130,2);
      } else if((temp%5)==0 && (temp%10)!=0) {
        spr.drawLine(i*8,170,i*8,150,lineColor);
        spr.drawLine((i*8)+1,170,(i*8)+1,150,lineColor);
        // spr.drawFloat(temp/10.0,1,i*8,144);        
      } else {
        spr.drawLine(i*8,170,i*8,160,lineColor);
      }
    }
  
   temp=temp+1;
  }

  if (currentMode == FM) {
    spr.fillSmoothRoundRect(240,20,76,22,4,TFT_WHITE);
    spr.fillSmoothRoundRect(241,21,74,20,4,TFT_BLACK);
    if (rx.getCurrentPilot()) {
      spr.setTextColor(TFT_GREEN,TFT_BLACK);
      spr.drawString("FM Stereo",278,31,2);
      spr.setTextColor(TFT_WHITE,TFT_BLACK);
    } else spr.drawString("FM Mono",278,31,2);
  } else {
    spr.fillSmoothRoundRect(240,20,76,22,4,TFT_WHITE);
    spr.fillSmoothRoundRect(241,21,74,20,4,TFT_BLACK);
    spr.drawString(bandModeDesc[currentMode],278,31,2);
  }
   
  
  // spr.setTextColor(TFT_MAGENTA,TFT_BLACK);
  spr.drawString(bufferStationName,160,102,4);
  // spr.setTextColor(TFT_WHITE,TFT_BLACK);

  // ---- stuck-button warning: the encoder still tunes, the button is ignored
  if (btnFaulty) {
    spr.setTextFont(0);
    spr.setTextDatum(ML_DATUM);
    spr.setTextColor(TFT_RED, TFT_BLACK);
    spr.drawString("BTN?", 82, 9);
    spr.setTextColor(TFT_WHITE, TFT_BLACK);
    spr.setTextDatum(MC_DATUM);
  }

  drawStatusIcons();

  // ---- network banner: portal instructions, or the CAT address for a few
  //      seconds after the link state changes. Drawn last so it wins.
  // (int32_t)(now - deadline) keeps working across the 49-day millis() wrap, which
  // a plain millis() < deadline comparison does not.
  if (netPortalActive() or (catNetBannerUntil && (int32_t)(millis() - catNetBannerUntil) < 0)) {
    uint16_t bg = netPortalActive() ? TFT_BLUE : 0x0208;
    spr.fillRect(0, 112, 320, 18, bg);
    spr.setTextFont(0);
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(TFT_WHITE, bg);
    String line;
    if (netPortalActive())
      // Font 0 is 6 px wide and the panel is 320 px, so keep this under ~52 chars.
      // Must be the soft-AP address: netIP() reports the station address once
      // the link is up, which is unreachable from a phone joined to the portal.
      line = String("WiFi setup: \"") + NET_AP_SSID + "\" -> " + netApIP();
    else
      line = String("CAT ") + netStatusDetail();
    spr.drawString(line.c_str(), 160, 121);
    spr.setTextColor(TFT_WHITE, TFT_BLACK);
  }

  // ---- transient confirmation banner
  if (toastUntil && (int32_t)(millis() - toastUntil) < 0) {
    spr.fillSmoothRoundRect(70,44,180,32,4,TFT_WHITE);
    spr.fillSmoothRoundRect(71,45,178,30,4,TFT_MENU_BACK);
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(TFT_WHITE,TFT_MENU_BACK);
    spr.drawString(toastMsg,160,60,2);
    spr.setTextColor(TFT_WHITE,TFT_BLACK);
  }

  // ---- destructive-action modal, drawn last so nothing can paint over it
  if (cmdConfirmReset) {
    spr.fillSmoothRoundRect(40,38,240,70,5,TFT_RED);
    spr.fillSmoothRoundRect(42,40,236,66,5,TFT_MENU_BACK);
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(TFT_WHITE,TFT_MENU_BACK);
    spr.drawString("Erase all settings?",160,56,2);
    spr.setTextFont(0);
    for (int i = 0; i < 2; i++) {
      bool sel = (confirmEraseSel == (i == 1));
      int x = 60 + i*110;
      uint16_t c = (i == 1) ? TFT_RED : 0x0400;
      if (sel) { spr.fillRoundRect(x,80,100,18,3,c); spr.setTextColor(TFT_WHITE,c); }
      else     { spr.drawRoundRect(x,80,100,18,3,0x8410); spr.setTextColor(0x8410,TFT_MENU_BACK); }
      spr.drawString(i == 1 ? "ERASE" : "Cancel", x+50, 89, 2);
    }
    spr.setTextColor(TFT_WHITE,TFT_BLACK);
  }

  batteryMonitor(true);   
  spr.pushSprite(0,0);
}

void cleanBfoRdsInfo()
{
  bufferStationName[0]='\0';
}

void showRDSMsg()
{
  rdsMsg[35] = bufferRdsMsg[35] = '\0';
  if (strcmp(bufferRdsMsg, rdsMsg) == 0)
    return;
}

void showRDSStation()
{
  if (strcmp(bufferStationName, stationName) == 0 ) return;
  cleanBfoRdsInfo();
  strcpy(bufferStationName, stationName);
  drawSprite();
}

void showRDSTime()
{

  if (strcmp(bufferRdsTime, rdsTime) == 0)
    return;
}

void checkRDS()
{
  rx.getRdsStatus();
  if (rx.getRdsReceived())
  {
    if (rx.getRdsSync() && rx.getRdsSyncFound())
    {
      rdsMsg = rx.getRdsText2A();
      stationName = rx.getRdsText0A();
      rdsTime = rx.getRdsTime();
      // if ( rdsMsg != NULL )   showRDSMsg();
      if (stationName != NULL)         
          showRDSStation();
      // if ( rdsTime != NULL ) showRDSTime();
    }
  }
}

/***************************************************************************************
** Function name:           DrawBatteryLevel
** Description:             Draw a battery level icon
***************************************************************************************/
// Draw a battery level icon
void DrawBatteryLevel(int batteryLevel) {

  int chargeLevel;
  uint16_t batteryLevelColor;

  if ( batteryLevel == 1 ) {
    chargeLevel=4;
    batteryLevelColor=TFT_RED;
  }
  if ( batteryLevel == 2 ) {
    chargeLevel=9;
    batteryLevelColor=TFT_YELLOW;
  }
  if ( batteryLevel == 3 ) {
    chargeLevel=13;
    batteryLevelColor=TFT_YELLOW;
  }
  if ( batteryLevel == 4 ) {
    chargeLevel=18;
    batteryLevelColor=TFT_GREEN;
  }
  if ( batteryLevel == 5 ) {   // To do: Animated icon to charge mode!
    chargeLevel=18;
    batteryLevelColor=TFT_GREEN;
  }
  spr.drawRect(XbatPos, YbatPos, Xbatsiz, Ybatsiz, TFT_WHITE);
  spr.fillRect(XbatPos+1, YbatPos+1, Xbatsiz-2, Ybatsiz-2, TFT_BLACK);
  spr.fillRect(XbatPos+1, YbatPos+1, chargeLevel, Ybatsiz-2, batteryLevelColor);
  spr.fillRect(XbatPos+20, YbatPos+2, 2, 5, TFT_WHITE);
  // No push here. drawSprite() calls batteryMonitor(true) immediately before its own
  // pushSprite(), so pushing from inside streamed the 108 KB sprite out of PSRAM
  // twice per frame -- and that push is what stalls the other core's flash fetches,
  // i.e. the CAT latency the rest of this file works to keep down. The standalone
  // caller in batteryMonitor() pushes for itself.
}

/***************************************************************************************
** Function name:           batteryMonitor
** Description:             Check Battery Level
***************************************************************************************/
// Check Battery Level
void batteryMonitor(bool forced) {
  // The sampling is throttled, the drawing is not. drawSprite() calls this with
  // forced=true on every frame, so reading the ADC here put two analogRead sweeps
  // directly on the CAT reply path -- for a value that cannot move meaningfully
  // inside a couple of seconds. The icon still repaints from the cached level on
  // every frame, so nothing on screen goes stale.
  static uint32_t lastSample = 0;
  bool changed = false;

  if (!lastSample or (millis() - lastSample) > 2000) {
    lastSample = millis();
    if (battery.getBatteryVolts() >= MIN_USB_VOLTAGE) {
      if (!batteryCharging) { batteryCharging = true; changed = true; }
      currentBatteryLevel = 5;
    } else {
      if (batteryCharging) { batteryCharging = false; changed = true; }
      int batteryLevel = battery.getBatteryChargeLevel();
      if(batteryLevel >=80){
        currentBatteryLevel = 4;
      }else if(batteryLevel >= 50 ){
        currentBatteryLevel = 3;
      }else if(batteryLevel >= 20 ){
        currentBatteryLevel = 2;
      }else{
        currentBatteryLevel = 1;
      }
    }
    if (currentBatteryLevel != previousBatteryLevel) changed = true;
  }

  if (changed or forced) {
    DrawBatteryLevel(currentBatteryLevel);
    previousBatteryLevel = currentBatteryLevel;
    // Only the standalone caller pushes: drawSprite() pushes the whole frame itself.
    if (!forced) spr.pushSprite(0,0);
  }
  yield();  
}


// ===========================================================================
//  CAT radio hooks
//
//  CatControl.cpp knows nothing about this sketch's globals; it reaches the
//  receiver only through the functions below (declared in CatControl.h).
// ===========================================================================

// The SI4735 library counts FM in 10 kHz units and everything else in kHz, so
// the conversion to Hz depends on the band type.
static uint32_t bandUnitHz(int idx)
{
  return (band[idx].bandType == FM_BAND_TYPE) ? 10000UL : 1000UL;
}

static bool bandCoversHz(int idx, uint32_t hz)
{
  uint32_t u = bandUnitHz(idx);
  return hz >= (uint32_t)band[idx].minimumFreq * u &&
         hz <= (uint32_t)band[idx].maximumFreq * u;
}

uint32_t catRadioGetFreqHz()
{
  int32_t hz = (int32_t)((uint32_t)currentFrequency * bandUnitHz(bandIdx));
  if (currentMode == LSB || currentMode == USB) hz += currentBFO;
  return (hz < 0) ? 0 : (uint32_t)hz;
}

bool catRadioSetFreqHz(uint32_t hz)
{
  if (!radioPresent) return false;

  // Stay on the current band when it can reach the frequency, so overlapping
  // shortwave bands do not swap around under the user.
  int target = bandCoversHz(bandIdx, hz) ? bandIdx : -1;
  if (target < 0)
    for (int i = 0; i <= lastBand; i++)
      if (bandCoversHz(i, hz)) { target = i; break; }

  if (target < 0) return false;   // no band reaches it -> CAT answers "N;"

  bool fmTarget = (band[target].bandType == FM_BAND_TYPE);
  bool ssb = !fmTarget && ssbLoaded && (currentMode == LSB || currentMode == USB);

  uint16_t chan;
  int16_t  newBfo = 0;

  if (fmTarget)
  {
    chan = (uint16_t)((hz + 5000UL) / 10000UL);      // nearest 10 kHz
  }
  else
  {
    chan = (uint16_t)(hz / 1000UL);
    int16_t rem = (int16_t)(hz % 1000UL);
    if (ssb)
    {
      // Keep the BFO near zero: 7 074 300 Hz becomes 7074 kHz with +300 Hz,
      // and 7 074 700 Hz becomes 7075 kHz with -300 Hz.
      if (rem > 500) { chan++; rem -= 1000; }
      newBfo = rem;
    }
  }

  if (chan < band[target].minimumFreq) chan = band[target].minimumFreq;
  if (chan > band[target].maximumFreq) chan = band[target].maximumFreq;

  if (target != bandIdx)
  {
    band[bandIdx].currentFreq = currentFrequency;
    band[bandIdx].currentStepIdx = currentStepIdx;
    bandIdx = target;
    band[bandIdx].currentFreq = chan;
    useBand();                        // reprograms the tuner, redraws, ~100 ms
  }
  else
  {
    rx.setFrequency(chan);
    currentFrequency = rx.getFrequency();
  }

  if (ssb)
  {
    currentBFO = newBfo;
    rx.setSSBBfo(currentBFO);
    // Keep the panel's BFO text in step; it is rendered straight from this buffer.
    sprintf(bfo, "%+4.4d", currentBFO);
  }

  if (currentMode == FM) cleanBfoRdsInfo();
  band[bandIdx].currentFreq = currentFrequency;
  catMarkDirty();
  return true;
}

uint8_t catRadioGetMode()
{
  return currentMode;
}

bool catRadioSetMode(uint8_t mode)
{
  // Not just wasted I2C: loadSSB() pushes ~15 KB of patch with a spin-wait per block,
  // which would hang the diagnostic halt for seconds against a bus nobody answers.
  if (!radioPresent) return false;

  // Clients re-send the current mode routinely; useBand() below is a full retune.
  if (mode == currentMode) return true;

  if (mode == FM)
  {
    if (band[bandIdx].bandType == FM_BAND_TYPE) return true;
    for (int i = 0; i <= lastBand; i++)
    {
      if (band[i].bandType == FM_BAND_TYPE)
      {
        band[bandIdx].currentFreq = currentFrequency;
        band[bandIdx].currentStepIdx = currentStepIdx;
        bandIdx = i;
        useBand();
        return true;
      }
    }
    return false;
  }

  // AM and SSB only exist below 30 MHz, so a request for either while the radio is
  // on the VHF band means leaving that band. Refusing outright was unhelpful -- the
  // client had no way to know it first had to send a frequency -- so hop to the last
  // AM-type band that was in use and apply the mode there.
  if (band[bandIdx].bandType == FM_BAND_TYPE)
  {
    band[bandIdx].currentFreq = currentFrequency;
    band[bandIdx].currentStepIdx = currentStepIdx;
    bandIdx = (lastAmBandIdx >= 0 && lastAmBandIdx <= lastBand &&
               band[lastAmBandIdx].bandType != FM_BAND_TYPE) ? lastAmBandIdx : 1;
    currentFrequency = band[bandIdx].currentFreq;
    currentStepIdx = band[bandIdx].currentStepIdx;
  }

  if (mode == AM)
  {
    currentMode = AM;
    bfoOn = ssbLoaded = false;
  }
  else if (mode == LSB || mode == USB)
  {
    if (!ssbLoaded) loadSSB();        // the SSB patch has to be re-uploaded
    currentMode = mode;
  }
  else
  {
    return false;
  }

  band[bandIdx].currentFreq = currentFrequency;
  band[bandIdx].currentStepIdx = currentStepIdx;
  useBand();

  // useBand() reprograms the tuner through rx.setSSB(), which clears the chip's
  // BFO while currentBFO still reports the old offset -- so catRadioGetFreqHz()
  // started lying by up to a kHz. Put it back.
  if (currentMode == LSB || currentMode == USB) rx.setSSBBfo(currentBFO);

  catMarkDirty();       // a CAT mode/band change deserves saving like any other
  return true;
}

uint8_t catRadioGetVolume()
{
  return rx.getVolume();
}

void catRadioSetVolume(uint8_t v)
{
  if (!radioPresent) return;
  if (v > 63) v = 63;
  rx.setVolume(v);
  catMarkDirty();
}

int catRadioGetRssiDbuv()
{
  // Keep a private cache. Writing the shared `rssi` global meant loop()'s
  // "redraw only when it changed" test (rssi != aux) was already satisfied by
  // CAT, so the on-screen S-meter froze for as long as a client kept polling.
  // Clients poll several times a second, so throttle the I2C read to 250 ms.
  static uint32_t lastRead = 0;
  static uint8_t  catRssi = 0;
  if (!radioPresent) return 0;      // no chip to read: do not stall on a dead bus
  if (millis() - lastRead > 250)
  {
    lastRead = millis();
    rx.getCurrentReceivedSignalQuality();
    catRssi = rx.getCurrentRSSI();
  }
  return (int)catRssi;
}

bool catRadioGetMute()
{
  return muted;
}

void catRadioSetMute(bool m)
{
  if (!radioPresent) return;
  muted = m;
  rx.setAudioMute(muted);
}

// Live GPIO readings, served over CAT as "ZZB;" and "ZZP;". The button pin has
// misbehaved in ways that contradict each other, and the display is a poor place
// to watch a signal, so expose the raw levels to the PC instead.
void catRadioDiag(char kind, char *out, size_t n)
{
  if (kind == 'B')
  {
    snprintf(out, n, " gpio%u lvl=%d seenHigh=%d faulty=%d lowFor=%lums",
             (unsigned)ENCODER_PUSH_BUTTON,
             digitalRead(ENCODER_PUSH_BUTTON),
             btnSeenReleased ? 1 : 0,
             btnFaulty ? 1 : 0,
             btnLowSince ? (unsigned long)(millis() - btnLowSince) : 0UL);
    return;
  }

  if (kind == 'P')
  {
    // Read-only. The first version called pinMode() on all of these from inside
    // the CAT parser, so any host on the LAN could reconfigure the UART and
    // strapping pins, and it slept 5 ms in the command path. Only the button pin
    // is deliberately configured (in setup), the rest are simply sampled.
    static const uint8_t pins[] = {0, 3, 5, 6, 7, 14, 21, 38, 39, 40, 41, 43, 44, 47, 48};
    const uint8_t nPins = sizeof(pins) / sizeof(pins[0]);
    size_t used = 0;
    for (uint8_t i = 0; i < nPins; i++)
    {
      int w = snprintf(out + used, (used < n) ? n - used : 0,
                       " %u=%d", pins[i], digitalRead(pins[i]));
      if (w > 0) used += (size_t)w;
    }
    return;
  }

  snprintf(out, n, " ?");
}

void catRadioTouched()
{
  noteInteraction();     // a remote command is real activity: wake the backlight
  catUiDirty = true;
}


/**
 * Main loop
 */
void loop()
{
  catServiceFromLoop();              // keep remote control responsive across the slow bits

  batteryMonitor(false);  // Battery check (samples at most once every 2 s)
  catServiceFromLoop();

  // Check if the encoder has moved.
  if (encoderCount != 0)
  {
    // Take the accumulated detents. The helpers below are all written around a
    // single +/-1, so pass them the direction and repeat, rather than handing them
    // a count they would misread as a direction.
    //
    // Consume at most a few per iteration and leave the rest in encoderCount:
    // zeroing it discarded everything past the cap, which is the loss the ISR
    // accumulator exists to prevent, and replaying a long burst here blocks the
    // loop (every helper redraws, and each redraw pushes the sprite twice) far
    // past the timeout a PC client allows.
    noInterrupts();
    int steps = encoderCount;
    int8_t dir = (steps > 0) ? 1 : -1;
    int n = steps > 0 ? steps : -steps;
    if (n > 4) n = 4;
    encoderCount -= dir * n;                  // leftovers handled next iteration
    interrupts();

    noteInteraction();
    triggerLedChase(dir);
    if (cmdConfirmReset)
    {
      confirmEraseSel = !confirmEraseSel;     // rotate to pick Cancel / ERASE
      drawSprite();
    }
    else if (bfoOn & (currentMode == LSB || currentMode == USB))
    {
      long bfo = (long)currentBFO + (long)dir * currentBFOStep * n;
      if (bfo >  16000) bfo =  16000;         // stay inside what the chip accepts
      if (bfo < -16000) bfo = -16000;
      currentBFO = (int16_t)bfo;
      rx.setSSBBfo(currentBFO);
      showBFO();
    }
    else if (cmdMenu)
      for (int k = 0; k < n; k++) doMenu(dir);
    else if (cmdMode)
      doMode(dir);                            // one step: may re-upload the SSB patch
    else if (cmdStep)
      for (int k = 0; k < n; k++) doStep(dir);
    else if (cmdAgc)
      for (int k = 0; k < n; k++) doAgc(dir);
    else if (cmdBandwidth)
      for (int k = 0; k < n; k++) doBandwidth(dir);
    else if (cmdVolume)
      for (int k = 0; k < n; k++) doVolume(dir);
    else if (cmdSoftMuteMaxAtt)
      for (int k = 0; k < n; k++) doSoftMute(dir);
    else if (cmdBand)
      setBand(dir);                           // one step: useBand() costs ~100 ms
    else
    {
      for (int k = 0; k < n; k++)
      {
        if (dir == 1) rx.frequencyUp();
        else          rx.frequencyDown();
      }
      if (currentMode == FM) cleanBfoRdsInfo();
      // Show the current frequency only if it has changed
      currentFrequency = rx.getFrequency();
      showFrequency();
    }
    resetEepromDelay();
    delay(MIN_ELAPSED_TIME);
    elapsedCommand = millis();
  }
  else
  {
    if (encoderButtonPressed())
    {
       noteInteraction();
       uint32_t timestamp = millis() + 3000;
        while (encoderButtonPressed()) {
          catServiceFromLoop();                     // stay answerable during a long press
          if ((int32_t)(millis() - timestamp) >= 0){
            tft.fillScreen(TFT_BLACK);
            tft.setTextSize(2);
            tft.setTextColor(TFT_WHITE, TFT_BLACK);
            tft.setTextDatum(MC_DATUM);
            tft.setCursor(tft.width()/2,tft.height()/2);
            tft.println("SLEEP");
            delay(3000);
            digitalWrite(PIN_POWER_ON, 0);
            // The wake pin needs its pull-up in RTC mode as well, otherwise it can
            // float low through sleep -- waking instantly, and still reading low on
            // the next boot.
            rtc_gpio_pullup_en((gpio_num_t)ENCODER_PUSH_BUTTON);
            rtc_gpio_pulldown_dis((gpio_num_t)ENCODER_PUSH_BUTTON);
            esp_sleep_enable_ext0_wakeup((gpio_num_t)ENCODER_PUSH_BUTTON, 0);
            esp_deep_sleep_start();
          }
      }
      timestamp = 0;
      countClick++;
      if (cmdConfirmReset)
      {
        // Checked before everything else so no other click path can reach the
        // wipe, and Cancel is what a stray double-click lands on.
        bool go = confirmEraseSel;
        cmdConfirmReset = false;
        confirmEraseSel = false;
        if (go) performConfigReset();          // does not return
        disableCommands();
        showStatus();
        drawSprite();
      }
      else if (cmdMenu)
      {
        currentMenuCmd = menuIdx;
        doCurrentMenuCmd();
      }
      else if (countClick == 1)
      { // If just one click, you can select the band by rotating the encoder
        if (isMenuMode())
        {
          disableCommands();
          showStatus();
          showCommandStatus((char *)"VFO ");
        }
        else if (bfoOn) {
          bfoOn = false;
          showStatus();
        }
        else
        {
          cmdBand = !cmdBand;
          // cmdMenu = !cmdMenu;
          menuIdx=BAND;          
          currentMenuCmd = menuIdx;
          drawSprite();
        }
      }
      else
      { // GO to MENU if more than one click in less than 1/2 seconds.
        cmdMenu = !cmdMenu;
        if (cmdMenu) {
          menuIdx = VOLUME;
          showMenu();
        }
      }
      delay(MIN_ELAPSED_TIME);
      elapsedCommand = millis();
    }
  }

  // Show RSSI status only if this condition has changed
  if ((millis() - elapsedRSSI) > MIN_ELAPSED_RSSI_TIME * 6)
  {
    rx.getCurrentReceivedSignalQuality();
    snr= rx.getCurrentSNR();
    int aux = rx.getCurrentRSSI();
    if (rssi != aux && !isMenuMode())
    {
      rssi = aux;
      showRSSI();
    }
    elapsedRSSI = millis();
  }
  catServiceFromLoop();

  // Disable commands control
  if ((millis() - elapsedCommand) > ELAPSED_COMMAND)
  {
    if ((currentMode == LSB || currentMode == USB) )
    {
      bfoOn = false;
      // showBFO();
      disableCommands();
      showStatus();
    } else if (cmdConfirmReset) {
      cmdConfirmReset = false;                 // timing out means Cancel
      showStatus();
      drawSprite();
    } else if (isMenuMode() or cmdBand) {
      disableCommands();
      showStatus();
    } 
    elapsedCommand = millis();
  }

  if ((millis() - elapsedClick) > ELAPSED_CLICK)
  {
    countClick = 0;
    elapsedClick = millis();
  }

  if ((millis() - lastRDSCheck) > RDS_CHECK_TIME) {
    if ((currentMode == FM) and (snr >= 12)) checkRDS();
    lastRDSCheck = millis();
  }  

  // Show the current frequency only if it has changed
  if (itIsTimeToSave)
  {
    if ((millis() - storeTime) > STORE_TIME)
    {
      saveAllReceiverInformation();
      storeTime = millis();
      itIsTimeToSave = false;
    }
  }

  // Remote control. netPoll() drives the WiFi state machine and the captive portal;
  // catServiceFromLoop() applies queued CAT sets and refreshes the snapshot the CAT
  // task answers from. Neither does socket I/O.
  // A USB host means the radio is tethered, so it does not need WiFi to be
  // controllable and the RF is better off quiet. Serial.isPlugged() watches USB
  // start-of-frame traffic, so it sees a real host and not a dumb charger. The
  // reading is known to flap, hence the settling window; the stored CAT WiFi
  // preference is untouched and resumes when the cable comes out.
  {
    static bool     usbHost = false;
    static bool     lastRead = false;
    static uint32_t stableSince = 0;
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
    bool now = Serial.isPlugged();
#else
    bool now = false;
#endif
    if (now != lastRead) { lastRead = now; stableSince = millis(); }
    else if (stableSince && (millis() - stableSince) > 2000 && usbHost != now)
    {
      usbHost = now;
      netSetSuspended(usbHost);
      catUiDirty = true;
    }
  }

  netPoll();
  catServiceFromLoop();

  if (netGetState() != catLastNetState)
  {
    catLastNetState = netGetState();
    catNetBannerUntil = millis() + 6000;
    catUiDirty = true;
  }

  // Keep the status icons honest: a client connecting or dropping changes them
  // without any local input, so poll those states as a redraw trigger too.
  //
  // Deliberately deferred by ~800 ms. Pushing the 108 KB sprite streams it out of
  // PSRAM, which stalls the other core's flash-cache fetches and so delays the CAT
  // task by tens of milliseconds. Redrawing the instant a client connects put that
  // stall exactly on top of the client's opening handshake -- Flrig waits a fixed
  // per-command time and read nothing, then declared the rig dead. Nobody needs the
  // badge to light up inside a second.
  {
    static bool lastU = false, lastN = false, lastAp = false;
    static uint32_t iconDirtyAt = 0;
    bool u = catUsbClientActive(), nn = catNetClientActive(), ap = netPortalActive();
    if (u != lastU || nn != lastN || ap != lastAp) {
      lastU = u; lastN = nn; lastAp = ap;
      iconDirtyAt = millis() + 800;
    }
    if (iconDirtyAt && (int32_t)(millis() - iconDirtyAt) >= 0) {
      iconDirtyAt = 0;
      catUiDirty = true;
    }
  }

  if (toastUntil && (int32_t)(millis() - toastUntil) >= 0) {
    toastUntil = 0;
    catUiDirty = true;                 // clear it once it has had its moment
  }

  if (catUiDirty && (millis() - catUiLastDraw) > 100)
  {
    catUiDirty = false;
    catUiLastDraw = millis();
    drawSprite();
    // Pushing the sprite costs tens of milliseconds. Service CAT again straight
    // afterwards so a command that landed mid-redraw is answered now rather than
    // a whole loop iteration later: Flrig waits a fixed tcpip_ping_delay per
    // command and treats a late reply as no reply at all.
    catServiceFromLoop();
  }

  updateScreenTimeout();
  updateLedRing();

  delay(5);
}
