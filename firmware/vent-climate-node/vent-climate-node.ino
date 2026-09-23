// ============================================================
//  vent-climate-node — ESP32-S3 damper controller for the F30 vent
//
//  WHAT IT DOES
//    Motorises the vent's OEM shutoff damper with a micro servo, driven by an
//    airstream temperature sensor:
//
//        air is COLD (AC running)  -> OPEN  the damper
//              cold air flows over the electronics = FREE ACTIVE COOLING
//        air is HOT  (heat running)-> CLOSE the damper
//              nothing hot reaches the Pi / Zynq stack
//
//    The inverted framing matters: the vent isn't only a hazard to shut off,
//    it's a cooling duct we can use whenever the AC is on.
//
//  FAIL-SAFE IS THE WHOLE POINT
//    Boot state is CLOSED. Any sensor fault forces CLOSED. If this node dies,
//    the damper must not be sitting open with the heater running. Everything
//    here is arranged so that "no information" == "closed".
//
//  NO EXTERNAL LIBRARIES
//    Servo driven straight from LEDC, thermistor read straight from the ADC.
//    Fewer dependencies = fewer things to break in a car at -10 C or +70 C.
// ============================================================

// ---------------- pins ----------------
#define PIN_SERVO        5      // servo signal (give the servo its OWN supply)
#define PIN_T_AIR        1      // ADC: airstream thermistor
#define PIN_T_CASE       2      // ADC: electronics enclosure thermistor (optional)
#define HAVE_CASE_SENSOR 1

// ---------------- thermistor ----------------
// Divider: 10k fixed from 3V3 -> node, 10k NTC from node -> GND.
//   R_ntc = R_FIXED * adc / (ADC_MAX - adc)
#define R_FIXED        10000.0f
#define NTC_R25        10000.0f     // NTC resistance at 25 C
#define NTC_BETA        3950.0f     // typical 10k NTC beta; check your part
#define ADC_MAX         4095.0f
#define ADC_SAMPLES       16        // averaged, ADC on ESP32 is noisy

// ---------------- control thresholds (deg C) ----------------
// Hysteresis band: without it the servo hunts every few seconds and dies early.
#define AIR_COLD_C      18.0f   // below this, air is cooling -> open
#define AIR_HOT_C       30.0f   // above this, heat is on -> close
#define CASE_WARM_C     45.0f   // enclosure this warm -> prefer open if air is cold
#define SENSOR_MIN_C   -40.0f   // plausibility window; outside = fault = CLOSED
#define SENSOR_MAX_C   125.0f

// ---------------- servo ----------------
// Travel is mechanism-specific. ### MEASURE ### against the real damper and set
// these so CLOSED truly seats and OPEN doesn't strain the linkage.
#define SERVO_US_CLOSED  1000
#define SERVO_US_OPEN    2000
#define SERVO_FREQ_HZ      50
#define SERVO_RES_BITS     16
#define SERVO_PERIOD_US 20000

// Detach (stop pulsing) once the move is done: a stalled hobby servo draws
// ~1 A and cooks itself. We only hold position while actually moving.
#define SERVO_SETTLE_MS   700

enum DamperState { DAMPER_CLOSED = 0, DAMPER_OPEN = 1 };
static DamperState g_state    = DAMPER_CLOSED;   // fail-safe boot state
static bool        g_fault    = true;            // until a good read proves otherwise
static float       g_tAir     = NAN;
static float       g_tCase    = NAN;
static uint32_t    g_lastMove = 0;
static bool        g_servoLive = false;

// ============================================================
//  servo
// ============================================================
static void servoWriteUs(int us) {
  uint32_t maxDuty = (1UL << SERVO_RES_BITS) - 1UL;
  uint32_t duty = (uint32_t)((float)us / (float)SERVO_PERIOD_US * (float)maxDuty);
  ledcWrite(PIN_SERVO, duty);
}

static void servoGo(DamperState s) {
  if (!g_servoLive) {
    ledcAttach(PIN_SERVO, SERVO_FREQ_HZ, SERVO_RES_BITS);
    g_servoLive = true;
  }
  servoWriteUs(s == DAMPER_OPEN ? SERVO_US_OPEN : SERVO_US_CLOSED);
  g_lastMove = millis();
}

// Release the servo after it has settled so a stalled horn can't cook it.
static void servoIdleCheck() {
  if (g_servoLive && (millis() - g_lastMove > SERVO_SETTLE_MS)) {
    ledcDetach(PIN_SERVO);
    g_servoLive = false;
  }
}

// ============================================================
//  thermistor
// ============================================================
static float readTempC(int pin) {
  uint32_t acc = 0;
  for (int i = 0; i < ADC_SAMPLES; i++) { acc += analogRead(pin); delayMicroseconds(200); }
  float adc = (float)acc / ADC_SAMPLES;

  // Open or shorted sensor lands at the rails -> report NAN, caller treats as fault.
  if (adc < 8.0f || adc > (ADC_MAX - 8.0f)) return NAN;

  float r = R_FIXED * adc / (ADC_MAX - adc);
  // Beta equation, 25 C reference
  float steinhart = logf(r / NTC_R25) / NTC_BETA + 1.0f / 298.15f;
  float tC = 1.0f / steinhart - 273.15f;
  if (tC < SENSOR_MIN_C || tC > SENSOR_MAX_C) return NAN;
  return tC;
}

// ============================================================
//  control
// ============================================================
static void controlTick() {
  g_tAir = readTempC(PIN_T_AIR);
#if HAVE_CASE_SENSOR
  g_tCase = readTempC(PIN_T_CASE);
#endif

  // ---- fail-safe: no trustworthy air reading -> CLOSED, unconditionally ----
  if (isnan(g_tAir)) {
    if (!g_fault || g_state != DAMPER_CLOSED) {
      g_fault = true;
      g_state = DAMPER_CLOSED;
      servoGo(g_state);
      Serial.println("FAULT air sensor -> damper CLOSED (fail-safe)");
    }
    return;
  }
  g_fault = false;

  DamperState want = g_state;               // default: hold, gives us hysteresis

  if (g_tAir >= AIR_HOT_C)        want = DAMPER_CLOSED;   // heat on -> protect
  else if (g_tAir <= AIR_COLD_C)  want = DAMPER_OPEN;     // AC on -> free cooling
  // between the thresholds: hold whatever we were doing (no hunting)

#if HAVE_CASE_SENSOR
  // If the stack is hot and the air is NOT hot, cooling wins.
  if (!isnan(g_tCase) && g_tCase >= CASE_WARM_C && g_tAir < AIR_HOT_C)
    want = DAMPER_OPEN;
#endif

  if (want != g_state) {
    g_state = want;
    servoGo(g_state);
    Serial.printf("air=%.1fC case=%.1fC -> damper %s\n",
                  g_tAir, g_tCase, g_state == DAMPER_OPEN ? "OPEN" : "CLOSED");
  }
}

// ============================================================
static void printStatus() {
  Serial.printf("STATUS air=%.1fC case=%.1fC damper=%s fault=%s servo=%s\n",
                g_tAir, g_tCase,
                g_state == DAMPER_OPEN ? "OPEN" : "CLOSED",
                g_fault ? "YES" : "no",
                g_servoLive ? "driven" : "released");
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);

  // Drive CLOSED before anything else can go wrong.
  servoGo(DAMPER_CLOSED);
  Serial.println("vent-climate-node: boot state = CLOSED (fail-safe).");
  Serial.println("  cold air -> OPEN (free cooling) | hot air -> CLOSED (protect)");
  Serial.println("  'i' status  'o' force open  'c' force close  'a' auto");
}

static bool g_manual = false;

void loop() {
  static uint32_t lastCtl = 0, lastPrint = 0;

  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == 'i') printStatus();
    else if (ch == 'o') { g_manual = true;  g_state = DAMPER_OPEN;   servoGo(g_state); Serial.println("manual OPEN"); }
    else if (ch == 'c') { g_manual = true;  g_state = DAMPER_CLOSED; servoGo(g_state); Serial.println("manual CLOSED"); }
    else if (ch == 'a') { g_manual = false; Serial.println("auto"); }
  }

  if (!g_manual && millis() - lastCtl > 2000) { lastCtl = millis(); controlTick(); }
  if (millis() - lastPrint > 10000) { lastPrint = millis(); printStatus(); }

  servoIdleCheck();
  delay(10);
}
