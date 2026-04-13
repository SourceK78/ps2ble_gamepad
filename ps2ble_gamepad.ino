/*******************************************************************************
 * PS2 BLE Gamepad
 *
 * Connects a PlayStation 1/2 wired controller via BLE HID gamepad.
 * Uses PsxNewLib (bitbang) and BleGamepad libraries.
 *
 * Original PsxNewLib: Copyright (C) 2019-2020 SukkoPera <software@sukkology.net>
 * Licensed under GNU GPL v3 or later. See http://www.gnu.org/licenses.
 *******************************************************************************/

#include <PsxControllerBitBang.h>
#include <BleGamepad.h>

#if defined(AVR)
#include <avr/pgmspace.h>
#else
#include <pgmspace.h>
#endif

// --- Pin assignments ---
const byte PIN_PS2_ATT = 5;
const byte PIN_PS2_CMD = 10;
const byte PIN_PS2_DAT = 9;
const byte PIN_PS2_CLK = 8;

const byte BUTTON_PIN = 3;
const byte LED_PIN    = 4;

// --- Timing ---
const unsigned long SLEEP_HOLD_MS      = 1000;
const unsigned long BATTERY_UPDATE_MS  = 10000;

// --- Battery ---
const float VBATT_MAX = 4.10f;
const float VBATT_MIN = 3.00f;

// --- Globals ---
PsxControllerBitBang<PIN_PS2_ATT, PIN_PS2_CMD, PIN_PS2_DAT, PIN_PS2_CLK> psx;
BleGamepad              bleGamepad("PS2 BLE Gamepad", "Espressif", 100);
BleGamepadConfiguration bleGamepadConfig;

bool          haveController  = false;
float         batteryLevel    = 1.0f;
unsigned long lastBatteryMs   = 0;
unsigned long sleepPressStart = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

int16_t mapAxis(byte val) {
  return (int16_t)map(val, 0, 255, -32767, 32767);
}

void updateButton(uint8_t bleBtn, bool pressed) {
  if (pressed) bleGamepad.press(bleBtn);
  else         bleGamepad.release(bleBtn);
}

signed char dpadToHat(bool up, bool right, bool down, bool left) {
  if (up    && right) return HAT_UP_RIGHT;
  if (right && down)  return HAT_DOWN_RIGHT;
  if (down  && left)  return HAT_DOWN_LEFT;
  if (left  && up)    return HAT_UP_LEFT;
  if (up)             return HAT_UP;
  if (right)          return HAT_RIGHT;
  if (down)           return HAT_DOWN;
  if (left)           return HAT_LEFT;
  return HAT_CENTERED;
}

// Returns battery level in [0.0, 1.0], quantised to 5% steps.
// Level is monotonically non-increasing to avoid spurious upward jumps.
float readBatteryLevel() {
  uint32_t adcSum = 0;
  for (int i = 0; i < 16; i++) {
    adcSum += analogReadMilliVolts(A0);
  }
  float v = 2.0f * adcSum / 16 / 1000.0f;
  v = constrain(v, VBATT_MIN, VBATT_MAX);
  float level = roundf((v - VBATT_MIN) / (VBATT_MAX - VBATT_MIN) / 0.05f) * 0.05f;
  if (level > batteryLevel) level = batteryLevel;  // only allow decreasing
  return level;
}

// ---------------------------------------------------------------------------
// Controller logic
// ---------------------------------------------------------------------------

bool tryConnectController() {
  if (!psx.begin()) return false;

  Serial.println(F("Controller found!"));
  delay(300);

  if (!psx.enterConfigMode()) {
    Serial.println(F("Cannot enter config mode"));
    return true;
  }

  if (!psx.enableAnalogSticks()) {
    Serial.println(F("Cannot enable analog sticks"));
  }
  if (!psx.enableAnalogButtons()) {
    Serial.println(F("Cannot enable analog buttons"));
  }
  if (!psx.exitConfigMode()) {
    Serial.println(F("Cannot exit config mode"));
  }
  return true;
}

void sendControllerState() {
  // Face buttons and shoulders
  updateButton(BUTTON_1,  psx.buttonPressed(PSB_CROSS));
  updateButton(BUTTON_2,  psx.buttonPressed(PSB_CIRCLE));
  updateButton(BUTTON_3,  psx.buttonPressed(PSB_SQUARE));
  updateButton(BUTTON_4,  psx.buttonPressed(PSB_TRIANGLE));
  updateButton(BUTTON_5,  psx.buttonPressed(PSB_L1));
  updateButton(BUTTON_6,  psx.buttonPressed(PSB_R1));
  updateButton(BUTTON_7,  psx.buttonPressed(PSB_L2));
  updateButton(BUTTON_8,  psx.buttonPressed(PSB_R2));
  updateButton(BUTTON_9,  psx.buttonPressed(PSB_SELECT));
  updateButton(BUTTON_10, psx.buttonPressed(PSB_START));
  updateButton(BUTTON_11, psx.buttonPressed(PSB_L3));
  updateButton(BUTTON_12, psx.buttonPressed(PSB_R3));

  // D-pad → HAT
  bleGamepad.setHat1(dpadToHat(
    psx.buttonPressed(PSB_PAD_UP),
    psx.buttonPressed(PSB_PAD_RIGHT),
    psx.buttonPressed(PSB_PAD_DOWN),
    psx.buttonPressed(PSB_PAD_LEFT)
  ));

  // Analog sticks
  byte lx, ly, rx, ry;
  psx.getLeftAnalog(lx, ly);
  psx.getRightAnalog(rx, ry);

  bleGamepad.setAxes(
    mapAxis(lx),  // X
    mapAxis(ly),  // Y
    mapAxis(rx),  // Z
    mapAxis(ry),  // Rz
    0, 0, 0, 0    // Rx, Ry, Slider1, Slider2 (unused)
  );

  bleGamepad.sendReport();
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  bleGamepadConfig.setAutoReport(false);
  bleGamepadConfig.setButtonCount(16);
  bleGamepadConfig.setHatSwitchCount(1);
  bleGamepad.begin(&bleGamepadConfig);

  pinMode(A0, INPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
    Serial.println(F("Woke from sleep"));
  }

  Serial.println(F("Ready!"));
  delay(2000);
}

void loop() {
  unsigned long now = millis();

  // Battery level update
  if (now - lastBatteryMs >= BATTERY_UPDATE_MS) {
    batteryLevel = readBatteryLevel();
    lastBatteryMs = now;
		bleGamepad.setBatteryLevel((uint8_t)(batteryLevel * 100.0));
  }

  // Wait for BLE connection
  if (!bleGamepad.isConnected()) {
    delay(200);
    return;
  }

  // Controller state machine
  if (!haveController) {
    haveController = tryConnectController();
  } else {
    if (!psx.read()) {
      Serial.println(F("Controller lost :("));
      haveController = false;
    } else {
      sendControllerState();
    }
  }

  // Deep sleep on button hold
  if (digitalRead(BUTTON_PIN) == LOW) {
    if (sleepPressStart == 0) {
      sleepPressStart = now;
    } else if (now - sleepPressStart >= SLEEP_HOLD_MS) {
      Serial.println(F("Going to deep sleep..."));
      delay(2000);
      while (digitalRead(BUTTON_PIN) == LOW) delay(10);
      delay(50);
      digitalWrite(LED_PIN, LOW);
      esp_deep_sleep_enable_gpio_wakeup(BIT(BUTTON_PIN), ESP_GPIO_WAKEUP_GPIO_LOW);
      esp_deep_sleep_start();
    }
  } else {
    sleepPressStart = 0;
  }

  delay(1000 / 60);
}
