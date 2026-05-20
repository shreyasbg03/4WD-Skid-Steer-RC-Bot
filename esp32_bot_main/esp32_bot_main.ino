// ============================================================
// 4WD Skid Steer RC Bot — ESP32 Firmware
// Raw iBUS parser — no external libraries required
// FlySky FS-i6X + FS-iA10B via iBUS single wire protocol
// ============================================================

// Motor pins — BTS7960 drivers
#define L_RPWM 18   // Left motors forward PWM
#define L_LPWM 19   // Left motors reverse PWM
#define R_RPWM 25   // Right motors forward PWM
#define R_LPWM 26   // Right motors reverse PWM
#define EN_PIN 21   // Enable — shared by both drivers

// RC Channels (1-indexed, matching transmitter assignment)
#define CH_THROTTLE  2   // Right stick vertical
#define CH_STEERING  1   // Right stick horizontal
#define CH_ARM       6   // Arm switch — bot moves only when armed
#define CH_SPEED     7   // Speed mode — 100% or 50% power
#define CH_INVERT    5   // Front/back invert — for flipped orientation

// ============================================================
// TUNING — adjust these to match your bot's feel
// ============================================================
#define DEADZONE        8     // Stick center deadzone in microseconds
                              // Increase if bot creeps at rest
#define PWM_FREQ        20000 // 20kHz — above audible range, quieter motors
#define PWM_RES         8     // 8-bit resolution — PWM range 0 to 255
#define CH_CENTER       1500  // iBUS center value in microseconds
#define CH_RANGE        500   // iBUS half-range (1000 to 2000us = ±500)
#define SPEED_FULL      255   // Max PWM at 100% speed mode
#define SPEED_HALF      127   // Max PWM at 50% speed mode
#define PWM_MIN_FULL    60    // Minimum PWM to overcome motor stall — full speed
                              // Increase if motors hum but don't move
#define PWM_MIN_HALF    40    // Minimum PWM to overcome motor stall — half speed
#define EXPO_BLEND      0.2f  // Expo curve blend — 0.0 = linear, 1.0 = full cubic
                              // Lower = more responsive at small stick movements
// ============================================================

// iBUS protocol constants
// Packet: 2 byte header (0x20 0x40) + 20 bytes channel data + 2 byte checksum = 32 bytes total
// Each channel is 2 bytes little-endian, range 1000–2000 microseconds
#define IBUS_CHANNELS  10
#define IBUS_LEN       32

uint16_t ibusChannels[IBUS_CHANNELS] = {1500,1500,1500,1500,1500,1500,1500,1500,1500,1500};

// ============================================================
// iBUS Parser — non-blocking, called every loop
// Syncs on header bytes 0x20 0x40, validates checksum,
// then extracts all 10 channel values
// ============================================================
void readIbus() {
  static uint8_t buf[IBUS_LEN];
  static uint8_t idx = 0;

  while (Serial2.available()) {
    uint8_t c = Serial2.read();

    // Sync on header — first byte must be 0x20
    if (idx == 0 && c != 0x20) continue;
    // Second byte must be 0x40
    if (idx == 1 && c != 0x40) { idx = 0; continue; }

    buf[idx++] = c;

    if (idx == IBUS_LEN) {
      idx = 0;

      // Validate checksum — sum of all bytes except last 2 subtracted from 0xFFFF
      uint16_t checksum = 0xFFFF;
      for (int i = 0; i < IBUS_LEN - 2; i++) checksum -= buf[i];
      uint16_t rxCheck = buf[IBUS_LEN-2] | (buf[IBUS_LEN-1] << 8);
      if (checksum != rxCheck) return;  // Bad packet — discard

      // Parse channels — 2 bytes each, little-endian, starting at byte index 2
      for (int ch = 0; ch < IBUS_CHANNELS; ch++) {
        ibusChannels[ch] = buf[2 + ch*2] | (buf[3 + ch*2] << 8);
      }
    }
  }
}

// Returns channel value by 1-indexed channel number
// Returns center value (1500) for out-of-range requests
uint16_t getChannel(int ch) {
  if (ch < 1 || ch > IBUS_CHANNELS) return CH_CENTER;
  return ibusChannels[ch - 1];
}

// ============================================================
// Expo curve — smooths low-speed stick response
// Full stick input still produces full output (preserves max speed)
// Blend between linear and cubic based on EXPO_BLEND factor
// ============================================================
int applyCurve(int pwmVal, int maxPWM) {
  if (maxPWM == 0 || pwmVal == 0) return 0;
  float n = (float)pwmVal / maxPWM;
  float curved = (1.0f - EXPO_BLEND) * n + EXPO_BLEND * (n * n * n);
  return (int)(curved * maxPWM);
}

// ============================================================
// Launch boost — ensures motors start moving immediately
// Values already above pwmMin pass through unchanged
// Values below pwmMin get bumped up to pwmMin
// Zero stays zero (full stop)
// ============================================================
int applyLaunchBoost(int pwmVal, int pwmMin) {
  if (pwmVal == 0) return 0;
  if (pwmVal >= pwmMin) return pwmVal;
  return pwmMin;
}

// Drive a single BTS7960 — direction determined by forward flag
void driveMotor(int rpwmPin, int lpwmPin, int pwm, bool forward) {
  if (forward) { ledcWrite(rpwmPin, pwm); ledcWrite(lpwmPin, 0); }
  else         { ledcWrite(rpwmPin, 0);   ledcWrite(lpwmPin, pwm); }
}

// Stop all motors immediately
void stopMotors() {
  ledcWrite(L_RPWM, 0);
  ledcWrite(L_LPWM, 0);
  ledcWrite(R_RPWM, 0);
  ledcWrite(R_LPWM, 0);
}

void setup() {
  Serial.begin(115200);

  // iBUS receiver on Serial2 — GPIO 16 = RX2
  Serial2.begin(115200, SERIAL_8N1, 16, 17);

  // Attach PWM to motor pins — ESP32 Arduino core v3.x API
  ledcAttach(L_RPWM, PWM_FREQ, PWM_RES);
  ledcAttach(L_LPWM, PWM_FREQ, PWM_RES);
  ledcAttach(R_RPWM, PWM_FREQ, PWM_RES);
  ledcAttach(R_LPWM, PWM_FREQ, PWM_RES);

  // Enable pin — starts LOW (disarmed)
  pinMode(EN_PIN, OUTPUT);
  digitalWrite(EN_PIN, LOW);

  // Initialize channel array to safe center values
  for (int i = 0; i < IBUS_CHANNELS; i++) ibusChannels[i] = 1500;

  stopMotors();
  Serial.println("Bot ready. Waiting for arm signal...");
}

void loop() {
  readIbus();  // Non-blocking — parse any available iBUS bytes

  uint16_t rawThrottle = getChannel(CH_THROTTLE);
  uint16_t rawSteering = getChannel(CH_STEERING);
  uint16_t rawArm      = getChannel(CH_ARM);
  uint16_t rawSpeed    = getChannel(CH_SPEED);
  uint16_t rawInvert   = getChannel(CH_INVERT);

  // Failsafe — if throttle is out of valid range, something is wrong
  if (rawThrottle < 800 || rawThrottle > 2200) {
    stopMotors();
    digitalWrite(EN_PIN, LOW);
    return;
  }

  // CH6 arm check — switch down (>1700us) = armed
  bool armed    = (rawArm > 1700);
  // CH5 invert — switch down = inverted mode (for flipped orientation)
  bool inverted = (rawInvert > 1700);

  // Disarm — stop everything
  if (!armed) {
    stopMotors();
    digitalWrite(EN_PIN, LOW);
    Serial.println("DISARMED");
    delay(50);
    return;
  }
  digitalWrite(EN_PIN, HIGH);  // Enable both BTS7960 drivers

  // CH7 speed mode — switch down = full power, up = half power
  int maxSpeed = (rawSpeed > 1700) ? SPEED_FULL : SPEED_HALF;
  int pwmMin   = (rawSpeed > 1700) ? PWM_MIN_FULL : PWM_MIN_HALF;

  // Center stick values around zero
  int throttle = (int)rawThrottle - CH_CENTER;  // positive = forward
  int steering = (int)rawSteering - CH_CENTER;  // positive = right

  // Apply deadzone
  if (abs(throttle) < DEADZONE) throttle = 0;
  if (abs(steering) < DEADZONE) steering = 0;

  // Invert both axes for flipped orientation
  // Negating throttle AND steering keeps forward/left/right all correct
  // when bot is physically rotated 180 degrees
  if (inverted) {
    throttle = -throttle;
    steering = steering;
  }

  // Skid steer mix — differential drive
  // Push right stick right: left motors get full power, right motors slow/reverse
  int leftPower  = throttle - steering;
  int rightPower = throttle + steering;

  // Throttle priority — rescale so the dominant side always reaches full range
  // Without this, forward + turn reduces max speed to ~80%
  int maxPower = max(abs(leftPower), abs(rightPower));
  if (maxPower > CH_RANGE) {
    float scale = (float)CH_RANGE / maxPower;
    leftPower  = (int)(leftPower  * scale);
    rightPower = (int)(rightPower * scale);
  }

  leftPower  = constrain(leftPower,  -CH_RANGE, CH_RANGE);
  rightPower = constrain(rightPower, -CH_RANGE, CH_RANGE);

  // Map power to PWM range
  int leftPWM  = map(abs(leftPower),  0, CH_RANGE, 0, maxSpeed);
  int rightPWM = map(abs(rightPower), 0, CH_RANGE, 0, maxSpeed);

  // Apply expo curve — smooth response near center, full power at extremes
  leftPWM  = applyCurve(leftPWM,  maxSpeed);
  rightPWM = applyCurve(rightPWM, maxSpeed);

  // Apply launch boost — ensures motors start immediately from standstill
  leftPWM  = applyLaunchBoost(leftPWM,  pwmMin);
  rightPWM = applyLaunchBoost(rightPWM, pwmMin);

  // Drive motors — direction determined by sign of power value
  driveMotor(L_RPWM, L_LPWM, leftPWM,  leftPower  >= 0);
  driveMotor(R_RPWM, R_LPWM, rightPWM, rightPower >= 0);

  // Debug output — comment out during competition to reduce loop overhead
  Serial.printf("ARM:%d SPD:%3d%% INV:%d | T:%4d S:%4d | L:%4d R:%4d\n",
                armed, (maxSpeed == SPEED_FULL ? 100 : 50), inverted,
                throttle, steering, leftPWM, rightPWM);

  delay(10);  // ~100Hz loop rate
}
