#include <avr/io.h>
#include <avr/sleep.h>
#include <EEPROM.h>

#include <ptc.h>
#include <tinyNeoPixel_Static.h>

static const uint8_t WAKE_BUTTON_PIN = PIN_PA3;
static const uint8_t LED_DATA_PIN = PIN_PB3;
static const uint8_t LED_POWER_PIN = PIN_PB2;

static const uint8_t NUM_LEDS = 18;
static const uint8_t LEDS_PER_EYE = 9;
static const uint8_t NUM_TOUCH_BUTTONS = 6;

static const uint32_t WAKE_TIMEOUT_MS = 900000UL;
static const uint16_t MAX_BUTTON_HOLD_MS = 2000;
static const uint16_t SHORT_PRESS_THRESHOLD_MS = 200;
static const uint16_t LONG_PRESS_THRESHOLD_MS = 1200;
static const uint8_t BUTTON_DEBOUNCE_MS = 50;
static const uint8_t TOUCH_POLL_MS = 10;
static const uint8_t SLEEP_DEBOUNCE_CYCLES = 3;
static const uint8_t FIND_SEQUENCE_LENGTH = 7;
static const uint8_t FIND_SEQUENCE_STARTING_LIVES = 10;
static const uint16_t FIND_SEQUENCE_PREVIEW_MS = 3000;


static const uint8_t LEFT_BLUE_MASK = 1 << 0;
static const uint8_t LEFT_RED_MASK = 1 << 1;
static const uint8_t LEFT_GREEN_MASK = 1 << 2;
static const uint8_t RIGHT_GREEN_MASK = 1 << 3;
static const uint8_t RIGHT_RED_MASK = 1 << 4;
static const uint8_t RIGHT_BLUE_MASK = 1 << 5;

struct RgbColor {
  uint8_t red;
  uint8_t green;
  uint8_t blue;
};

static const RgbColor COLOR_OFF = {0, 0, 0};
static const RgbColor COLOR_RED = {30, 0, 0};
static const RgbColor COLOR_GREEN = {0, 30, 0};
static const RgbColor COLOR_BLUE = {0, 0, 30};

static const uint8_t TOUCH_BUTTON_PINS[NUM_TOUCH_BUTTONS] = {
  PIN_PA4,
  PIN_PA5,
  PIN_PA6,
  PIN_PA7,
  PIN_PB0,
  PIN_PB1,
};

cap_sensor_t touchButtons[NUM_TOUCH_BUTTONS];
byte pixelBuffer[NUM_LEDS * 3];
tinyNeoPixel ledStrip = tinyNeoPixel(NUM_LEDS, LED_DATA_PIN, NEO_GRB, pixelBuffer);

volatile bool shouldProcessPtc = false;
volatile bool rebootOnButtonPress = false;

uint8_t state;
static uint8_t gAnimMode;
static uint8_t artieForced = 0;
static uint8_t artieTouchLatch = 0;

uint16_t randomState = 0xACE1u;

void seedGameRandom()
{
  randomState ^= (uint16_t)millis();
  if (randomState == 0) {
    randomState = 0xACE1u;
  }
}

uint8_t nextRandomByte()
{
  const bool bit = randomState & 1;
  randomState >>= 1;

  if (bit) {
    randomState ^= 0xB400u;
  }

  return (uint8_t)randomState;
}

uint8_t randomColorIndex()
{
  uint8_t value;
  do {
    value = nextRandomByte() & 0x03;
  } while (value > 2);

  return value;
}

uint8_t randomEight()
{
  return nextRandomByte() & 0x07;
}


uint8_t getPressedTouchMask()
{
  uint8_t pressedMask = 0;

  for (uint8_t buttonIndex = 0; buttonIndex < NUM_TOUCH_BUTTONS; ++buttonIndex) {
    if (ptc_get_node_touched(&touchButtons[buttonIndex])) {
      pressedMask |= (1 << buttonIndex);
    }
  }

  return pressedMask;
}

void setupTouchButtons()
{
  for (uint8_t buttonIndex = 0; buttonIndex < NUM_TOUCH_BUTTONS; ++buttonIndex) {
    const uint8_t result = ptc_add_selfcap_node(
      &touchButtons[buttonIndex],
      0,
      PIN_TO_PTC(TOUCH_BUTTON_PINS[buttonIndex])
    );

    if (result != PTC_LIB_SUCCESS) {
      while (true) { }
    }

    ptc_node_set_gain(&touchButtons[buttonIndex], PTC_GAIN_1);
    ptc_node_set_prescaler(&touchButtons[buttonIndex], PTC_PRESC_DIV4_gc);
    ptc_node_set_oversamples(&touchButtons[buttonIndex], 4);
    ptc_node_set_thresholds(&touchButtons[buttonIndex], 80, 10);
  }
}

void setAllLeds(uint8_t red, uint8_t green, uint8_t blue, bool show = false)
{
  for (uint8_t ledIndex = 0; ledIndex < NUM_LEDS; ++ledIndex) {
    ledStrip.setPixelColor(ledIndex, red, green, blue);
  }

  if (show) {
    ledStrip.show();
  }
}

void setAllLeds(RgbColor color, bool show = false)
{
  setAllLeds(color.red, color.green, color.blue, show);
}

void setTouchButtonPins(uint8_t mode)
{
  for (uint8_t buttonIndex = 0; buttonIndex < NUM_TOUCH_BUTTONS; ++buttonIndex) {
    pinMode(TOUCH_BUTTON_PINS[buttonIndex], mode);
  }
}

void disableRtc()
{
  RTC.PITINTCTRL = ~(RTC_PI_bm);
  shouldProcessPtc = false;
}

void enableRtcPtc(uint8_t period = RTC_PERIOD_CYC16_gc)
{
  RTC.PITINTCTRL = RTC_PI_bm;
  RTC.PITCTRLA = period | RTC_PITEN_bm;
  shouldProcessPtc = true;
}

void initRtc()
{
  while (RTC.STATUS > 0) {
    ;
  }

  RTC.CLKSEL = RTC_CLKSEL_INT1K_gc;
}

void enableRebootOnButton()
{
  PORTA.PIN3CTRL = PORT_PULLUPEN_bm | PORT_ISC_LEVEL_gc;
  rebootOnButtonPress = true;
}

void disableRebootOnButton()
{
  PORTA.PIN3CTRL = PORT_PULLUPEN_bm;
  rebootOnButtonPress = false;
}

void miniSleep(uint8_t period = RTC_PERIOD_CYC16_gc)
{
  RTC.PITINTCTRL = RTC_PI_bm;
  RTC.PITCTRLA = period | RTC_PITEN_bm;

  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  sleep_enable();
  sleep_cpu();

  RTC.PITINTCTRL = ~(RTC_PI_bm);
}

void artieCloseEyes()
{
  for (uint8_t f = 0; f < 70; f++) {
    setAllLeds(COLOR_OFF);
    if (f < 11) {
      // full teal (still awake) — hardcoded because ARTIE_CALM_COL defined later
      setAllLeds(0, 18, 16);
    } else if (f >= 15 && f < 51) {
      // lower arc blue pulse with decaying envelope
      uint8_t local = f - 15;
      uint8_t base = 30 - (local * 5) / 6;
      uint8_t wave = local % 18;
      uint8_t mod;
      if (wave <= 9) {
        mod = (wave * 8) / 9;
      } else {
        mod = ((17 - wave) * 8) / 9;
      }
      int8_t val = (int8_t)base - 4 + (int8_t)mod;
      uint8_t bright = val < 0 ? 0 : (val > 30 ? 30 : (uint8_t)val);
      for (uint8_t i = 3; i <= 6; i++) {
        ledStrip.setPixelColor(i, 0, 0, bright);
        ledStrip.setPixelColor(i + 9, 0, 0, bright);
      }
    }
    // frames 11-14: off (blink), frames 51-54: off (blink), frames 55-69: off (asleep)
    ledStrip.show();
    delay(80);
  }
}

void enterSleep()
{
  if (gAnimMode == 0) {
    artieCloseEyes();
  }

  disableRtc();
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);

  PORTA.PIN3CTRL = PORT_PULLUPEN_bm | PORT_ISC_LEVEL_gc;
  digitalWrite(LED_POWER_PIN, LOW);
  ADC0.CTRLA &= ~ADC_ENABLE_bm;
  setTouchButtonPins(OUTPUT);

  sleep_enable();
  sleep_cpu();

  // Wake is handled by rebooting through the watchdog, as in the original code.
  _PROTECTED_WRITE(WDT.CTRLA, WDT_PERIOD_8CLK_gc);
  while (true) { }
}

ISR(PORTA_PORT_vect)
{
  PORTA.INTFLAGS = PORT_INT3_bm;

  if (rebootOnButtonPress) {
    _PROTECTED_WRITE(WDT.CTRLA, WDT_PERIOD_8CLK_gc);
    while (true) { }
  }
}

ISR(RTC_PIT_vect)
{
  RTC.PITINTFLAGS = RTC_PI_bm;

  if (shouldProcessPtc) {
    ptc_process(millis());
  }
}

uint16_t measureWakeButtonLowTime(uint16_t maxMs)
{
  if (digitalRead(WAKE_BUTTON_PIN) == HIGH) {
    return 0;
  }

  delay(BUTTON_DEBOUNCE_MS);
  uint16_t elapsedMs = BUTTON_DEBOUNCE_MS;

  while (digitalRead(WAKE_BUTTON_PIN) == LOW) {
    delay(5);
    elapsedMs += 5;

    if (elapsedMs > maxMs) {
      return maxMs;
    }
  }

  return elapsedMs;
}

uint8_t loopingEyes(uint16_t step, uint8_t red, uint8_t green, uint8_t blue)
{
  setAllLeds(COLOR_OFF);

  const uint8_t ledIndex = step % 9;
  ledStrip.setPixelColor(ledIndex, red, green, blue);
  ledStrip.setPixelColor(17 - ledIndex, red, green, blue);
  ledStrip.show();

  return 100;
}

void setLeftEye(uint8_t red, uint8_t green, uint8_t blue, uint8_t offset = 0)
{
  for (uint8_t ledIndex = offset; ledIndex < 9 + offset; ++ledIndex) {
    ledStrip.setPixelColor(ledIndex, red, green, blue);
  }
}

void setRightEye(uint8_t red, uint8_t green, uint8_t blue)
{
  setLeftEye(red, green, blue, 9);
}

void setRightEyeLed(uint8_t ledIndex, uint8_t red, uint8_t green, uint8_t blue)
{
  ledStrip.setPixelColor(ledIndex == 0 ? 17 : ledIndex + 8, red, green, blue);
}

void showTouchedPads()
{
  if (gAnimMode == 0 && artieForced != 0) return;
  const uint8_t pressedMask = getPressedTouchMask();

  if (pressedMask & (LEFT_BLUE_MASK | LEFT_RED_MASK | LEFT_GREEN_MASK)) {
    setLeftEye(
      (pressedMask & LEFT_RED_MASK) ? 20 : 0,
      (pressedMask & LEFT_GREEN_MASK) ? 20 : 0,
      (pressedMask & LEFT_BLUE_MASK) ? 20 : 0
    );
  }

  if (pressedMask & (RIGHT_BLUE_MASK | RIGHT_RED_MASK | RIGHT_GREEN_MASK)) {
    setRightEye(
      (pressedMask & RIGHT_RED_MASK) ? 20 : 0,
      (pressedMask & RIGHT_GREEN_MASK) ? 20 : 0,
      (pressedMask & RIGHT_BLUE_MASK) ? 20 : 0
    );
  }

  ledStrip.show();
}

void flashNotification(RgbColor color)
{
  setAllLeds(color, true);
  delay(50);
  setAllLeds(COLOR_OFF, true);
  delay(50);
  setAllLeds(color, true);
  delay(50);
  setAllLeds(COLOR_OFF, true);
  delay(50);
}

void showFailure()
{
  flashNotification(COLOR_RED);
}

void showSuccess()
{
  
  EEPROM.update(0, state);
  for (uint8_t flashIndex = 0; flashIndex < 6; ++flashIndex) {
    flashNotification(COLOR_GREEN);
  }
}

bool isAnyPadPressed()
{
  return getPressedTouchMask() != 0;
}

void waitForAllTouchPadsReleased()
{
  while (getPressedTouchMask() != 0) {
    delay(TOUCH_POLL_MS);
  }
}

void waitForWakeButtonReleased()
{
  while (digitalRead(WAKE_BUTTON_PIN) == LOW) {
    delay(5);
  }
}

static const uint8_t MEMORY_START_LEVEL = 3;

RgbColor colorForIndex(uint8_t colorIndex)
{
  switch (colorIndex) {
    case 0:
      return COLOR_RED;
    case 1:
      return COLOR_GREEN;
    case 2:
      return COLOR_BLUE;
    default:
      return COLOR_OFF;
  }
}

bool playStopTheLightLevel(uint8_t iterationIntervalMs)
{
  const uint8_t targetLed = randomEight();

  for (uint8_t round = 0; round < 7; ++round) {
    for (uint8_t ledIndex = 0; ledIndex < 9; ++ledIndex) {
      setAllLeds(COLOR_OFF);
      ledStrip.setPixelColor(targetLed, 0, 30, 0);
      setRightEyeLed(targetLed, 0, 30, 0);
      ledStrip.setPixelColor(ledIndex, 30, 0, 0);
      setRightEyeLed(ledIndex, 30, 0, 0);
      ledStrip.show();

      uint8_t elapsedMs = 0;
      while (elapsedMs < iterationIntervalMs) {
        elapsedMs += 5;

        if (isAnyPadPressed()) {
          return ledIndex == targetLed;
        }

        delay(5);
      }
    }
  }

  return false;
}

bool playStopTheLight()
{
  enableRebootOnButton();
  for (uint8_t intervalMs = 200; intervalMs > 50; intervalMs -= 20) {
    if (!playStopTheLightLevel(intervalMs)) {
      showFailure();
      disableRebootOnButton();
      return false;
    }

    while (isAnyPadPressed()) {
      delay(5);
    }
  }

  state = state & B11111110;
  showSuccess();
  disableRebootOnButton();
  return true;
}

bool isExpectedSequenceButton(uint8_t expectedColor, uint8_t pressedMask)
{
  switch (expectedColor) {
    case 0:
      return pressedMask == LEFT_RED_MASK || pressedMask == RIGHT_RED_MASK;
    case 1:
      return pressedMask == LEFT_GREEN_MASK || pressedMask == RIGHT_GREEN_MASK;
    case 2:
      return pressedMask == LEFT_BLUE_MASK || pressedMask == RIGHT_BLUE_MASK;
    default:
      return false;
  }
}

void showSequenceColor(uint8_t colorIndex)
{
  setAllLeds(colorForIndex(colorIndex));
}

static const uint8_t NUM_SEQUENCE_LEVELS = 10;

void createRandomSequence(uint8_t sequence[], uint8_t sequenceLength)
{
  for (uint8_t sequenceIndex = 0; sequenceIndex < sequenceLength; ++sequenceIndex) {
    sequence[sequenceIndex] = randomColorIndex();
  }
}

bool playFollowTheSequence()
{
  enableRebootOnButton();
  uint8_t sequence[NUM_SEQUENCE_LEVELS];
  createRandomSequence(sequence, NUM_SEQUENCE_LEVELS);

  for (uint8_t level = MEMORY_START_LEVEL; level < NUM_SEQUENCE_LEVELS; ++level) {
    for (uint8_t sequenceIndex = 0; sequenceIndex < level; ++sequenceIndex) {
      showSequenceColor(sequence[sequenceIndex]);
      ledStrip.show();
      delay(400);
      setAllLeds(COLOR_OFF, true);
      delay(200);
    }

    for (uint8_t sequenceIndex = 0; sequenceIndex < level; ++sequenceIndex) {
      uint8_t pressedMask = getPressedTouchMask();

      while (pressedMask == 0) {
        pressedMask = getPressedTouchMask();
        delay(TOUCH_POLL_MS);
      }

      showTouchedPads();

      while (isAnyPadPressed()) {
        delay(TOUCH_POLL_MS);
      }

      if (!isExpectedSequenceButton(sequence[sequenceIndex], pressedMask)) {
        showFailure();
        disableRebootOnButton();
        return false;
      }

      setAllLeds(COLOR_OFF, true);
      delay(500);
    }
  }

  state = state & B11111101;
  showSuccess();
  disableRebootOnButton();
  return true;
}

bool pressedMaskToColorIndex(uint8_t pressedMask, uint8_t &colorIndex)
{
  switch (pressedMask) {
    case LEFT_RED_MASK:
    case RIGHT_RED_MASK:
      colorIndex = 0;
      return true;
    case LEFT_GREEN_MASK:
    case RIGHT_GREEN_MASK:
      colorIndex = 1;
      return true;
    case LEFT_BLUE_MASK:
    case RIGHT_BLUE_MASK:
      colorIndex = 2;
      return true;
    default:
      return false;
  }
}

void setSequenceSlotColor(uint8_t slotIndex, uint8_t colorIndex)
{
  const RgbColor color = colorForIndex(colorIndex);
  ledStrip.setPixelColor(slotIndex, color.red, color.green, color.blue);
}

void showFindSequenceProgress(const uint8_t sequence[], uint8_t foundLength)
{
  setAllLeds(COLOR_OFF);

  for (uint8_t sequenceIndex = 0; sequenceIndex < foundLength; ++sequenceIndex) {
    setSequenceSlotColor(sequenceIndex, sequence[sequenceIndex]);
  }

  ledStrip.show();
}

void showFindSequencePreview(const uint8_t sequence[])
{
  setAllLeds(COLOR_OFF);

  for (uint8_t sequenceIndex = 0; sequenceIndex < FIND_SEQUENCE_LENGTH; ++sequenceIndex) {
    setSequenceSlotColor(sequenceIndex, sequence[sequenceIndex]);
  }

  ledStrip.show();
  delay(FIND_SEQUENCE_PREVIEW_MS);
  showFindSequenceProgress(sequence, 0);
}

bool waitForColorGuess(uint8_t &colorIndex)
{
  uint8_t pressedMask = getPressedTouchMask();

  while (pressedMask == 0) {
    pressedMask = getPressedTouchMask();
    delay(TOUCH_POLL_MS);
  }

  showTouchedPads();
  const bool isKnownColor = pressedMaskToColorIndex(pressedMask, colorIndex);
  waitForAllTouchPadsReleased();

  return isKnownColor;
}

bool playFindTheSequence(uint8_t startingLives = FIND_SEQUENCE_STARTING_LIVES)
{
  enableRebootOnButton();
  uint8_t sequence[FIND_SEQUENCE_LENGTH];
  createRandomSequence(sequence, FIND_SEQUENCE_LENGTH);

  uint8_t livesRemaining = startingLives;
  uint8_t foundLength = 0;
  showFindSequencePreview(sequence);

  while (foundLength < FIND_SEQUENCE_LENGTH && livesRemaining > 0) {
    uint8_t guessedColor = 0;
    const bool hasColorGuess = waitForColorGuess(guessedColor);

    if (hasColorGuess && guessedColor == sequence[foundLength]) {
      ++foundLength;
      showFindSequenceProgress(sequence, foundLength);
      delay(500);
      continue;
    }

    --livesRemaining;
    foundLength = 0;
    showFailure();
    showFindSequenceProgress(sequence, foundLength);
  }

  const bool wonGame = foundLength == FIND_SEQUENCE_LENGTH;

  if (wonGame) {
    state = state & B11111011;
    showSuccess();
  }

  disableRebootOnButton();
  return wonGame;
}

// --- Artie Alive mode ---

enum : uint8_t { ARTIE_WAKEUP, ARTIE_IDLE, ARTIE_POWERDOWN };
enum : uint8_t { EMOTE_CALM, EMOTE_BLINK, EMOTE_LOOK, EMOTE_SCAN, EMOTE_HAPPY, EMOTE_WINK, EMOTE_SLEEPY, EMOTE_DBLBLINK, EMOTE_THINKING, EMOTE_SUSPICIOUS, EMOTE_STARTLED, EMOTE_ALERT, EMOTE_EXCITED };

struct ArtieState {
  uint8_t lifecycle;
  uint8_t emote;
  uint8_t subStep;
  uint8_t duration;
  uint8_t arg;
};
static ArtieState artie;

static const RgbColor ARTIE_CALM_COL  = { 0, 18, 16};
static const RgbColor ARTIE_SCAN_COL  = {28, 13,  0};
static const RgbColor ARTIE_SLEEP_COL = { 0,  0, 30};
static const RgbColor ARTIE_HAPPY_COL = { 0, 24,  4};
static const RgbColor ARTIE_WINK_COL  = {28, 13, 21};

static const uint16_t MASK_LOWER  = 0x078;  // bits 3,4,5,6
static const uint16_t MASK_TOP    = 0x187;  // bits 0,1,2,7,8
static const uint16_t MASK_LOOK_L = 0x1E0;  // bits 5,6,7,8
static const uint16_t MASK_LOOK_R = 0x01E;  // bits 1,2,3,4
static const uint16_t MASK_FULL   = 0x1FF;  // bits 0-8

uint8_t artieAliveMode(uint16_t step)
{
  if (step == 0) {
    artie.lifecycle = ARTIE_WAKEUP;
    artie.emote = EMOTE_CALM;
    artie.subStep = 0;
    artie.duration = 0;
    artie.arg = 0;
    artieForced = 0;
    artieTouchLatch = 0;
  }

  if (artie.lifecycle == ARTIE_WAKEUP) {
    setAllLeds(COLOR_OFF);

    if (step >= 8 && step < 48) {
      // Lower arc blue pulsing: 3 cycles over 40 frames
      uint8_t local = (step - 8) % 13;
      uint8_t bright;
      if (local <= 6) {
        bright = 4 + (local * 26) / 6;
      } else {
        bright = 4 + ((12 - local) * 26) / 6;
      }
      for (uint8_t i = 3; i <= 6; i++) {
        ledStrip.setPixelColor(i, 0, 0, bright);
        ledStrip.setPixelColor(i + 9, 0, 0, bright);
      }
    } else if (step >= 52 && step < 68) {
      setAllLeds(ARTIE_CALM_COL);
    } else if (step >= 72 && step < 80) {
      for (uint8_t i = 0; i < 9; i++) {
        if (MASK_LOOK_L & (1 << i)) {
          ledStrip.setPixelColor(i, ARTIE_CALM_COL.red, ARTIE_CALM_COL.green, ARTIE_CALM_COL.blue);
          ledStrip.setPixelColor(i + 9, ARTIE_CALM_COL.red, ARTIE_CALM_COL.green, ARTIE_CALM_COL.blue);
        }
      }
    } else if (step >= 80 && step < 88) {
      for (uint8_t i = 0; i < 9; i++) {
        if (MASK_LOOK_R & (1 << i)) {
          ledStrip.setPixelColor(i, ARTIE_CALM_COL.red, ARTIE_CALM_COL.green, ARTIE_CALM_COL.blue);
          ledStrip.setPixelColor(i + 9, ARTIE_CALM_COL.red, ARTIE_CALM_COL.green, ARTIE_CALM_COL.blue);
        }
      }
    } else if (step >= 88) {
      setAllLeds(ARTIE_CALM_COL);
    }
    // frames 0-7: off, 48-51: off(blink), 68-71: off(blink)

    if (step >= 100) {
      artie.lifecycle = ARTIE_IDLE;
      artie.emote = EMOTE_CALM;
      artie.subStep = 0;
      artie.duration = 0;
      artie.arg = 0;
    }
  } else {
    // ARTIE_IDLE

    // --- Touch reaction detection ---
    uint8_t tm = getPressedTouchMask();
    if (tm == 0) {
      artieTouchLatch = 0;
    } else if (tm != artieTouchLatch) {
      uint8_t nf = 0;
      if (tm & LEFT_BLUE_MASK) nf = 1;
      else if (tm & RIGHT_BLUE_MASK) nf = 2;
      else if (tm & LEFT_RED_MASK) nf = 3;
      else if (tm & RIGHT_RED_MASK) nf = 4;
      else if (tm & LEFT_GREEN_MASK) nf = 5;
      else if (tm & RIGHT_GREEN_MASK) nf = 6;
      if (nf != 0 && nf != artieForced) {
        artieForced = nf;
        artieTouchLatch = tm;
        artie.subStep = 0;
        if (nf == 1) artie.emote = EMOTE_SLEEPY;
        else if (nf == 2) { artie.emote = EMOTE_WINK; artie.arg = nextRandomByte() & 0x01; }
        else if (nf == 3) artie.emote = EMOTE_ALERT;
        else if (nf == 4) artie.emote = EMOTE_SUSPICIOUS;
        else if (nf == 5) artie.emote = EMOTE_EXCITED;
        else artie.emote = EMOTE_HAPPY;
      }
    }

    // --- Normal idle chooser (skipped when forced) ---
    if (artieForced == 0 && artie.duration == 0) {
      uint8_t r = nextRandomByte();
      if (r < 40) {
        artie.emote = EMOTE_BLINK;
        artie.duration = 3 + (nextRandomByte() & 0x03);
      } else if (r < 59) {
        artie.emote = EMOTE_DBLBLINK;
        artie.duration = 10;
      } else if (r < 108) {
        artie.emote = EMOTE_LOOK;
        artie.duration = 55 + (nextRandomByte() & 0x3F);
        uint8_t dir = nextRandomByte() & 0x01;
        uint8_t r2 = nextRandomByte();
        uint8_t mood = r2 < 180 ? 0 : (r2 < 240 ? 1 : 2);
        artie.arg = dir | (mood << 1);
      } else if (r < 148) {
        artie.emote = EMOTE_SCAN;
        artie.duration = 60 + (nextRandomByte() & 0x3F);
        artie.arg = nextRandomByte() % 9;
      } else if (r < 174) {
        artie.emote = EMOTE_HAPPY;
        artie.duration = 38 + (nextRandomByte() & 0x3F);
      } else if (r < 188) {
        artie.emote = EMOTE_SLEEPY;
        artie.duration = 55 + (nextRandomByte() & 0x3F);
      } else if (r < 208) {
        artie.emote = EMOTE_WINK;
        artie.duration = 24;
        artie.arg = nextRandomByte() & 0x01;
      } else if (r < 228) {
        artie.emote = EMOTE_THINKING;
        artie.duration = 40 + (nextRandomByte() & 0x3F);
      } else if (r < 244) {
        artie.emote = EMOTE_SUSPICIOUS;
        artie.duration = 30 + (nextRandomByte() & 0x1F);
      } else if (r < 250) {
        artie.emote = EMOTE_ALERT;
        artie.duration = 50 + (nextRandomByte() & 0x1F);
      } else {
        artie.emote = EMOTE_STARTLED;
        artie.duration = 28;
      }
      artie.subStep = 0;
    }

    // --- Render current emote (shared by idle and forced) ---
    setAllLeds(COLOR_OFF);
    uint16_t mask = 0;
    uint8_t cr = 0, cg = 0, cb = 0;

    if (artie.emote == EMOTE_CALM) {
      setAllLeds(ARTIE_CALM_COL);
    } else if (artie.emote == EMOTE_DBLBLINK) {
      if ((artie.subStep >= 2 && artie.subStep < 4) || artie.subStep >= 6) {
        setAllLeds(ARTIE_CALM_COL);
      }
    } else if (artie.emote == EMOTE_STARTLED) {
      if (artie.subStep < 6) setAllLeds(ARTIE_SCAN_COL);
      else if (artie.subStep < 16) setAllLeds(COLOR_RED);
      else if (artie.subStep >= 21) setAllLeds(ARTIE_CALM_COL);
    } else if (artie.emote == EMOTE_ALERT) {
      uint8_t local = artie.subStep & 0x1F;
      uint8_t tri = local <= 16 ? local : 32 - local;
      uint8_t bright = 8 + ((tri * 22) >> 4);
      for (uint8_t i = 0; i < NUM_LEDS; i++) ledStrip.setPixelColor(i, bright, 0, 0);
    } else if (artie.emote == EMOTE_EXCITED) {
      // Green top/full bounce every 5 frames
      mask = (((artie.subStep / 5) & 1) == 0) ? MASK_TOP : MASK_FULL;
      cr = ARTIE_HAPPY_COL.red; cg = ARTIE_HAPPY_COL.green; cb = ARTIE_HAPPY_COL.blue;
    } else if (artie.emote == EMOTE_SCAN) {
      uint8_t chunkOff = artie.subStep & 0x0F;
      if (artie.subStep > 0 && chunkOff == 0) {
        artie.arg = nextRandomByte() % 9;
      }
      uint8_t base = artie.arg;
      int8_t sweepOff = 0;
      if (chunkOff < 4) sweepOff = -1;
      else if (chunkOff >= 8 && chunkOff < 12) sweepOff = 1;
      uint8_t p = (base + 9 + sweepOff) % 9;
      uint8_t a = (p + 8) % 9;
      uint8_t b = (p + 1) % 9;
      ledStrip.setPixelColor(p, ARTIE_SCAN_COL.red, ARTIE_SCAN_COL.green, ARTIE_SCAN_COL.blue);
      ledStrip.setPixelColor(a, ARTIE_SCAN_COL.red, ARTIE_SCAN_COL.green, ARTIE_SCAN_COL.blue);
      ledStrip.setPixelColor(b, ARTIE_SCAN_COL.red, ARTIE_SCAN_COL.green, ARTIE_SCAN_COL.blue);
      ledStrip.setPixelColor(p + 9, ARTIE_SCAN_COL.red, ARTIE_SCAN_COL.green, ARTIE_SCAN_COL.blue);
      ledStrip.setPixelColor(a + 9, ARTIE_SCAN_COL.red, ARTIE_SCAN_COL.green, ARTIE_SCAN_COL.blue);
      ledStrip.setPixelColor(b + 9, ARTIE_SCAN_COL.red, ARTIE_SCAN_COL.green, ARTIE_SCAN_COL.blue);
    } else if (artie.emote == EMOTE_THINKING) {
      uint8_t phase = (artie.subStep >> 2) % 5;
      uint8_t center;
      if (phase == 0 || phase == 4) center = 8;
      else if (phase == 1 || phase == 3) center = 0;
      else center = 1;
      uint8_t ta = (center + 8) % 9;
      uint8_t tb = (center + 1) % 9;
      ledStrip.setPixelColor(center, ARTIE_SCAN_COL.red, ARTIE_SCAN_COL.green, ARTIE_SCAN_COL.blue);
      ledStrip.setPixelColor(ta, ARTIE_SCAN_COL.red, ARTIE_SCAN_COL.green, ARTIE_SCAN_COL.blue);
      ledStrip.setPixelColor(tb, ARTIE_SCAN_COL.red, ARTIE_SCAN_COL.green, ARTIE_SCAN_COL.blue);
      ledStrip.setPixelColor(center + 9, ARTIE_SCAN_COL.red, ARTIE_SCAN_COL.green, ARTIE_SCAN_COL.blue);
      ledStrip.setPixelColor(ta + 9, ARTIE_SCAN_COL.red, ARTIE_SCAN_COL.green, ARTIE_SCAN_COL.blue);
      ledStrip.setPixelColor(tb + 9, ARTIE_SCAN_COL.red, ARTIE_SCAN_COL.green, ARTIE_SCAN_COL.blue);
    } else {
      if (artie.emote == EMOTE_LOOK) {
        mask = (artie.arg & 1) ? MASK_LOOK_R : MASK_LOOK_L;
        uint8_t mood = (artie.arg >> 1) & 0x03;
        uint8_t q = artie.duration >> 2;
        RgbColor lc = ARTIE_CALM_COL;
        if (mood == 1) {
          if (artie.subStep >= 2*q && artie.subStep < 3*q) lc = ARTIE_SCAN_COL;
        } else if (mood == 2) {
          uint8_t h = q >> 1;
          if (artie.subStep >= q+h && artie.subStep < 2*q) lc = ARTIE_SCAN_COL;
          else if (artie.subStep >= 3*q && artie.subStep < 3*q+h) lc = COLOR_RED;
        }
        cr = lc.red; cg = lc.green; cb = lc.blue;
      } else if (artie.emote == EMOTE_HAPPY) {
        mask = MASK_TOP;
        cr = ARTIE_HAPPY_COL.red; cg = ARTIE_HAPPY_COL.green; cb = ARTIE_HAPPY_COL.blue;
      } else if (artie.emote == EMOTE_SLEEPY) {
        mask = MASK_LOWER;
        uint8_t wave = artie.subStep & 0x1F;
        if (wave > 16) wave = 32 - wave;
        cb = 8 + wave;
      } else if (artie.emote == EMOTE_SUSPICIOUS) {
        mask = ((artie.subStep >> 3) & 1) ? MASK_LOOK_R : MASK_LOOK_L;
        cr = ARTIE_SCAN_COL.red; cg = ARTIE_SCAN_COL.green; cb = ARTIE_SCAN_COL.blue;
      } else if (artie.emote == EMOTE_WINK) {
        cr = ARTIE_WINK_COL.red; cg = ARTIE_WINK_COL.green; cb = ARTIE_WINK_COL.blue;
        uint8_t winkEye = artie.arg & 1;
        uint8_t winkPhase;
        if (artieForced == 2) {
          // Forced wink: 60-frame held cycle
          uint8_t cycle = artie.subStep % 60;
          winkPhase = (cycle >= 25 && cycle < 35) ? 1 : 0;
        } else {
          // Idle wink: single 24-frame play
          winkPhase = (artie.subStep >= 8 && artie.subStep < 16) ? 1 : 0;
        }
        if (winkPhase) {
          uint8_t openOfs = winkEye ? 0 : 9;
          uint8_t shutOfs = winkEye ? 9 : 0;
          for (uint8_t i = 0; i < 9; i++) {
            ledStrip.setPixelColor(i + openOfs, cr, cg, cb);
            if (MASK_LOWER & (1 << i)) ledStrip.setPixelColor(i + shutOfs, cr, cg, cb);
          }
        } else {
          for (uint8_t i = 0; i < 9; i++) {
            ledStrip.setPixelColor(i, cr, cg, cb);
            ledStrip.setPixelColor(i + 9, cr, cg, cb);
          }
        }
      }
      if (artie.emote != EMOTE_WINK) {
        for (uint8_t i = 0; i < 9; i++) {
          if (mask & (1 << i)) {
            ledStrip.setPixelColor(i, cr, cg, cb);
            ledStrip.setPixelColor(i + 9, cr, cg, cb);
          }
        }
      }
    }

    // --- Step advancement ---
    artie.subStep++;
    if (artieForced != 0) {
      if (artie.subStep >= 240) artie.subStep = 0;
    } else if (artie.subStep >= artie.duration) {
      if (artie.emote != EMOTE_CALM) {
        artie.emote = EMOTE_CALM;
        artie.subStep = 0;
        artie.duration = 25 + (nextRandomByte() & 0x2F);
      } else {
        artie.duration = 0;
      }
    }
  }

  ledStrip.show();
  return 80;
}

uint8_t breath(uint16_t step, uint8_t r, uint8_t g, uint8_t b )
{
  step = step % 60;
  if (step > 30)
  {
    step = 60 - step;
  }
  setAllLeds(r * step, g * step, b * step, true);
  if (step == 0 )
  {
    return 200;
  }
  return 30;
}




int runAnimationMode(uint8_t mode, uint16_t step)
{
  switch (mode) {
    case 0:
      return artieAliveMode(step);
    case 1:
      return breath(step, 1, 0 , 0);
    case 2:
      return loopingEyes(step, 0, 0, 10);
    default:
      return -1;
  }
}

void handleWakeButtonPress(
  uint16_t heldMs,
  uint8_t &animationMode,
  uint16_t &animationStep,
  uint32_t &totalIntervalMs
)
{
  if (heldMs > LONG_PRESS_THRESHOLD_MS) {
    setAllLeds(COLOR_RED, true);
    waitForWakeButtonReleased();

    for (uint8_t cycle = 0; cycle < SLEEP_DEBOUNCE_CYCLES; ++cycle) {
      miniSleep();
    }

    enterSleep();
  } else {
    const uint8_t pressedMask = getPressedTouchMask();
    waitForAllTouchPadsReleased();

    if (pressedMask != 0) {
      seedGameRandom();
    }

    switch (pressedMask) {
      case 0:
        if (gAnimMode == 0 && artieForced != 0) {
          // Exit forced reaction, return to calm gap
          artieForced = 0;
          artieTouchLatch = 0;
          artie.emote = EMOTE_CALM;
          artie.subStep = 0;
          artie.duration = 25 + (nextRandomByte() & 0x2F);
          artie.arg = 0;
        } else {
          ++animationMode;
          gAnimMode = animationMode;
        }
        break;
      case LEFT_BLUE_MASK:
        artieForced = 0; artieTouchLatch = 0;
        playStopTheLight();
        break;
      case LEFT_RED_MASK:
        artieForced = 0; artieTouchLatch = 0;
        playFindTheSequence();
        break;
      case LEFT_GREEN_MASK:
        artieForced = 0; artieTouchLatch = 0;
        playFollowTheSequence();
        break;
    }
  }

  animationStep = 0;
  totalIntervalMs = 0;
}

void setup()
{
  pinMode(PIN_PA1, OUTPUT);
  pinMode(PIN_PA2, OUTPUT);
  pinMode(PIN_PB2, OUTPUT);
  pinMode(PIN_PB3, OUTPUT);

  pinMode(LED_DATA_PIN, OUTPUT);
  pinMode(WAKE_BUTTON_PIN, INPUT_PULLUP);
  digitalWrite(LED_POWER_PIN, HIGH);

  initRtc();
  setupTouchButtons();

  state = EEPROM.read(0);
}

void loop()
{
  ledStrip.begin();
  enableRtcPtc();

  uint16_t animationStep = 0;
  uint8_t animationMode = 5;
  gAnimMode = animationMode;
  uint32_t totalIntervalMs = 0;

  while (true) {
    int intervalMs = runAnimationMode(animationMode, animationStep);

    if (intervalMs == 0) {
      animationMode++;
      gAnimMode = animationMode;
    }

    if (intervalMs < 0) {
      animationMode = 0;
      gAnimMode = 0;
      continue;
    }

    ++animationStep;
    totalIntervalMs += intervalMs;

    while (intervalMs > 0) {
      showTouchedPads();
      delay(TOUCH_POLL_MS);
      intervalMs -= TOUCH_POLL_MS;

      const uint16_t buttonLowTime = measureWakeButtonLowTime(MAX_BUTTON_HOLD_MS);
      if (buttonLowTime > SHORT_PRESS_THRESHOLD_MS) {
        handleWakeButtonPress(buttonLowTime, animationMode, animationStep, totalIntervalMs);
      }
      else if (buttonLowTime > 0) {
        animationStep = 0;
      }
    }

    if (totalIntervalMs > WAKE_TIMEOUT_MS) {
      totalIntervalMs = 0;
      animationStep = 0;
      enterSleep();
    }
  }
}
