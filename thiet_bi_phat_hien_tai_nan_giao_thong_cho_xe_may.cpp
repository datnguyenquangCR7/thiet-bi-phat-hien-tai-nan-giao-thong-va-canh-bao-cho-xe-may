/*
  ESP32 + MPU6050 + A7680C + NEO-7M + INA3221 (robtillaart/INA3221 @ ^0.4.2) + Blynk
  PlatformIO / Arduino framework

  Virtual pins:
    V0 = Button: tắt cảnh báo tai nạn / dựng xe lên
    V1 = Text: hiển thị góc + g
    V2 = Button: bật/tắt chế độ khóa xe
    V3 = Text: trạng thái
    V4 = Text: link Google Maps
    V5 = Number: pin chính % (CH1+)
    V6 = Number: pin phụ % (CH2+)

  Replace all TEXT placeholders.
*/
#define BLYNK_TEMPLATE_ID "TMPL6Cl5BKyzM"
#define BLYNK_TEMPLATE_NAME "TEST"
#define BLYNK_AUTH_TOKEN "Ov7N7aHPk0X77LIqRkxK2vjvvFf3j12P"
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <TinyGPSPlus.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <BlynkSimpleEsp32.h>
#include "INA3221.h"

// -------------------- USER CONFIG --------------------
char BLYNK_AUTH[] = "Ov7N7aHPk0X77LIqRkxK2vjvvFf3j12P";
char WIFI_SSID[]  = "haha";
char WIFI_PASS[]  = "quangdatnguyen";

const char OWNER_NUMBER[] = "0869381546";
const char RELATIVE_1[]   = "TEXT";
const char RELATIVE_2[]   = "TEXT";
const char RELATIVE_3[]   = "TEXT";
const char RELATIVE_4[]   = "TEXT";
const char RELATIVE_5[]   = "TEXT";

// -------------------- PINS --------------------
static const int I2C_SDA = 21;
static const int I2C_SCL = 19;

static const int RELAY_BUZZER_PIN = 25;
static const bool RELAY_ACTIVE_LOW = false;  // đổi thành true nếu relay kích mức LOW

static const int A7680C_RX = 27;  // ESP32 RX  <- TX modem
static const int A7680C_TX = 32;  // ESP32 TX  -> RX modem
static const int GPS_RX     = 16;   // ESP32 RX  <- TX GPS
static const int GPS_TX     = 17;   // ESP32 TX  -> RX GPS

static const int MPU_INT_PIN = 5; // đặt chân INT nếu muốn wake từ deep sleep

// -------------------- SERIAL / SENSORS --------------------
HardwareSerial ModemSerial(1);
HardwareSerial GpsSerial(2);

Adafruit_MPU6050 mpu;
TinyGPSPlus gps;
INA3221 ina3221(0x40, &Wire);
BlynkTimer timer;

// -------------------- STATES --------------------
bool lockMode = false;
bool theftMode = false;
bool theftTriggered = false;
bool theftBaseFixValid = false;
double theftBaseLat = 0.0;
double theftBaseLon = 0.0;
unsigned long lastTheftSmsMs = 0;

bool accidentTriggered = false;
bool accidentConfirmed = false;
bool accidentModeMoving = false;
bool accidentModeStopped = false;
unsigned long movingImpactStartMs = 0;
unsigned long stoppedTiltStartMs = 0;
unsigned long movingCountdownStartMs = 0;
unsigned long stoppedCancelWindowStartMs = 0;
bool waitingOwnerCancel = false;

bool deepSleepEnabled = false;
unsigned long stableStartMs = 0;
float lastStableG = 0.0f;
float lastStableTilt = 0.0f;
float baselineRollDeg = 0.0f;
float baselinePitchDeg = 0.0f;

bool mainBatteryWarned = false;
bool auxBatteryWarned = false;
unsigned long lastBatteryWarnMs = 0;

// -------------------- HELPERS --------------------
static float rad2deg(float r) { return r * 180.0f / PI; }

static String makeMapsLink(double lat, double lon) {
  return String("https://maps.google.com/?q=") + String(lat, 6) + "," + String(lon, 6);
}

static double deg2rad(double d) { return d * PI / 180.0; }

static double distanceMeters(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371000.0;
  const double dLat = deg2rad(lat2 - lat1);
  const double dLon = deg2rad(lon2 - lon1);
  const double a = sin(dLat / 2.0) * sin(dLat / 2.0) +
                   cos(deg2rad(lat1)) * cos(deg2rad(lat2)) *
                   sin(dLon / 2.0) * sin(dLon / 2.0);
  const double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
  return R * c;
}

void relayBuzzer(bool on) {
  if (RELAY_ACTIVE_LOW) {
    digitalWrite(RELAY_BUZZER_PIN, on ? LOW : HIGH);
  } else {
    digitalWrite(RELAY_BUZZER_PIN, on ? HIGH : LOW);
  }
}

void setStatus(const String &msg) {
  Blynk.virtualWrite(V3, msg);
}

void modemSend(const String &cmd, uint16_t waitMs = 400) {
  ModemSerial.print(cmd);
  ModemSerial.print("\r\n");
  delay(waitMs);
}

bool waitModemToken(const char *token, uint32_t timeoutMs = 4000) {
  String buf;
  const uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    while (ModemSerial.available()) {
      char c = (char)ModemSerial.read();
      buf += c;
      if (buf.indexOf(token) >= 0) return true;
    }
    delay(10);
  }
  return false;
}

bool sendSMS(const char *number, const String &message) {
  if (!number || String(number) == "TEXT" || strlen(number) < 5) return false;

  modemSend("AT+CMGF=1", 200);
  ModemSerial.print("AT+CMGS=\"");
  ModemSerial.print(number);
  ModemSerial.print("\"\r\n");
  delay(600);
  ModemSerial.print(message);
  ModemSerial.write(26); // Ctrl+Z
  return waitModemToken("OK", 15000) || waitModemToken("+CMGS", 15000);
}

bool makeCall(const char *number, uint32_t ringMs = 15000) {
  if (!number || String(number) == "TEXT" || strlen(number) < 5) return false;

  ModemSerial.print("ATD");
  ModemSerial.print(number);
  ModemSerial.print(";\r\n");
  delay(ringMs);
  modemSend("ATH", 300);
  return true;
}

void smsAllRelatives(const String &message) {
  const char *nums[] = {RELATIVE_1, RELATIVE_2, RELATIVE_3, RELATIVE_4, RELATIVE_5};
  for (const char *n : nums) {
    if (n && String(n) != "TEXT" && strlen(n) >= 5) {
      sendSMS(n, message);
      delay(1000);
    }
  }
}

String currentGpsLink() {
  if (gps.location.isValid() && gps.location.age() < 10000) {
    return makeMapsLink(gps.location.lat(), gps.location.lng());
  }
  if (theftBaseFixValid) {
    return makeMapsLink(theftBaseLat, theftBaseLon);
  }
  return String("https://maps.google.com/?q=0,0");
}

void captureTheftBase() {
  if (gps.location.isValid() && gps.location.age() < 10000) {
    theftBaseLat = gps.location.lat();
    theftBaseLon = gps.location.lng();
    theftBaseFixValid = true;
    Blynk.virtualWrite(V4, makeMapsLink(theftBaseLat, theftBaseLon));
    setStatus("Da khoa vi tri cua xe");
  } else {
    theftBaseFixValid = false;
    setStatus("Chua co GPS fix de khoa vi tri");
  }
}

void resetTheftMode() {
  theftMode = false;
  theftTriggered = false;
  theftBaseFixValid = false;
  lastTheftSmsMs = 0;
  Blynk.virtualWrite(V2, 0);
  setStatus("Da tat che do deepsleep");
}

void resetAccidentMode() {
  accidentTriggered = false;
  accidentConfirmed = false;
  accidentModeMoving = false;
  accidentModeStopped = false;
  movingImpactStartMs = 0;
  stoppedTiltStartMs = 0;
  movingCountdownStartMs = 0;
  stoppedCancelWindowStartMs = 0;
  waitingOwnerCancel = false;
  relayBuzzer(false);
}

void enterDeepSleep() {
  deepSleepEnabled = true;
  setStatus("Da bat che do deepsleep");
  relayBuzzer(false);
  delay(300);

  esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
  if (MPU_INT_PIN >= 0) {
    pinMode(MPU_INT_PIN, INPUT_PULLUP);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)MPU_INT_PIN, 1);
  }
  esp_deep_sleep_start();
}

void takeOrientationBaseline() {
  sensors_event_t a, g, t;
  if (mpu.getEvent(&a, &g, &t)) {
    const float ax = a.acceleration.x / 9.80665f;
    const float ay = a.acceleration.y / 9.80665f;
    const float az = a.acceleration.z / 9.80665f;
    baselineRollDeg = rad2deg(atan2f(ay, az));
    baselinePitchDeg = rad2deg(atan2f(-ax, sqrtf(ay * ay + az * az)));
    lastStableG = sqrtf(ax * ax + ay * ay + az * az);
    lastStableTilt = 0.0f;
  }
}

float batteryPercentFromVoltage(float v) {
  // 2S Li-ion: 6.0V ~ 0%, 8.4V ~ 100%
  if (v <= 6.0f) return 0.0f;
  if (v >= 8.4f) return 100.0f;
  return (v - 6.0f) * 100.0f / (8.4f - 6.0f);
}

void sendBatteryWarnings(float mainPct, float auxPct) {
  const uint32_t now = millis();
  if (now - lastBatteryWarnMs < 300000UL) return; // chống spam 5 phút

  if (mainPct < 20.0f && !mainBatteryWarned) {
    mainBatteryWarned = true;
    sendSMS(OWNER_NUMBER, "CANH BAO: Pin chinh duoi 20%. Hay sac pin.");
    lastBatteryWarnMs = now;
  }
  if (auxPct < 20.0f && !auxBatteryWarned) {
    auxBatteryWarned = true;
    sendSMS(OWNER_NUMBER, "CANH BAO: Pin phu duoi 20%. Hay sac pin.");
    lastBatteryWarnMs = now;
  }

  if (mainPct >= 22.0f) mainBatteryWarned = false;
  if (auxPct >= 22.0f) auxBatteryWarned = false;
}

void processSmsCommand(String text, const String &sender) {
  text.trim();
  text.toUpperCase();

  if (text == "G") {
    theftMode = true;
    theftTriggered = false;
    captureTheftBase();
    sendSMS(sender.c_str(), "Da bat che do tim xe.");
    Blynk.virtualWrite(V2, 1);
    return;
  }

  if (text == "T") {
    resetTheftMode();
    sendSMS(sender.c_str(), "Da tat che do tim xe.");
    return;
  }

  if (text == "D") {
    if (accidentTriggered || waitingOwnerCancel) {
      resetAccidentMode();
      setStatus("Da dung xe len");
      sendSMS(sender.c_str(), "Da tat canh bao tai nan.");
    }
    return;
  }
}

// -------------------- GSM SMS PARSER --------------------
void pollModemForSms() {
  static String rx;
  while (ModemSerial.available()) {
    rx += char(ModemSerial.read());
    if (rx.length() > 3000) rx.remove(0, 1500);
  }

  // Parse incoming SMS format: +CMT: "number",...
  int p = rx.indexOf("+CMT:");
  if (p < 0) return;

  int lineEnd = rx.indexOf('\n', p);
  if (lineEnd < 0) return;

  String header = rx.substring(p, lineEnd);
  String sender = "TEXT";
  int q1 = header.indexOf('"');
  int q2 = header.indexOf('"', q1 + 1);
  if (q1 >= 0 && q2 > q1) sender = header.substring(q1 + 1, q2);

  int bodyStart = lineEnd + 1;
  int bodyEnd = rx.indexOf('\n', bodyStart);
  String body = (bodyEnd > bodyStart) ? rx.substring(bodyStart, bodyEnd) : rx.substring(bodyStart);

  processSmsCommand(body, sender);
  rx = "";
}

// -------------------- THEFT LOGIC --------------------
void handleTheft() {
  if (!theftMode || !theftBaseFixValid) return;
  if (!gps.location.isValid() || gps.location.age() > 15000) return;

  const double d = distanceMeters(theftBaseLat, theftBaseLon, gps.location.lat(), gps.location.lng());
  if (d <= 15.0) return;

  if (!theftTriggered) {
    theftTriggered = true;
    setStatus("Phat hien mat xe");
    makeCall(OWNER_NUMBER);
    sendSMS(OWNER_NUMBER, String("CANH BAO MAT XE! Vi tri: ") + currentGpsLink());
    lastTheftSmsMs = millis();
  } else if (millis() - lastTheftSmsMs >= 30000UL) {
    sendSMS(OWNER_NUMBER, String("Cap nhat vi tri xe: ") + currentGpsLink());
    lastTheftSmsMs = millis();
  }
}

// -------------------- ACCIDENT LOGIC --------------------
void alertAccidentToFamily(const String &modeLabel) {
  const String msg = String("CANH BAO TAI NAN (") + modeLabel + ")\nVi tri: " + currentGpsLink();
  makeCall(OWNER_NUMBER);
  sendSMS(OWNER_NUMBER, msg);
  smsAllRelatives(msg);
}

void handleAccident(float gMag, float tiltDeg) {
  if (accidentConfirmed) return;

  const bool strongImpact = (gMag > 4.0f);
  const bool tiltEnough = (tiltDeg > 45.0f);
  const bool movingAccidentCondition = strongImpact && tiltEnough;
  const bool stoppedAccidentCondition = tiltEnough && !strongImpact;

  // xe đang di chuyển: cần cả lực g và góc nghiêng trong 3 giây liên tục
  if (movingAccidentCondition) {
    if (movingImpactStartMs == 0) movingImpactStartMs = millis();
    if (!accidentTriggered && millis() - movingImpactStartMs >= 3000UL) {
      accidentTriggered = true;
      accidentModeMoving = true;
      waitingOwnerCancel = true;
      movingCountdownStartMs = millis();
      relayBuzzer(true);
      setStatus("Phat hien tai nan");
    }
  } else {
    movingImpactStartMs = 0;
  }

  // xe đang dừng / đi chậm: chỉ xét góc nghiêng trong 1 phút 30 giây
  if (stoppedAccidentCondition) {
    if (stoppedTiltStartMs == 0) stoppedTiltStartMs = millis();
    if (!accidentTriggered && millis() - stoppedTiltStartMs >= 90000UL) {
      accidentTriggered = true;
      accidentModeStopped = true;
      waitingOwnerCancel = true;
      stoppedCancelWindowStartMs = millis();
      relayBuzzer(true);
      setStatus("Phat hien tai nan");
    }
  } else {
    stoppedTiltStartMs = 0;
  }

  // nếu đã vào chế độ tai nạn khi xe đang chạy: còi kêu trong 90 giây, nhận D thì dừng
  if (accidentTriggered && accidentModeMoving && !accidentConfirmed) {
    relayBuzzer(true);
    if (millis() - movingCountdownStartMs >= 90000UL) {
      accidentConfirmed = true;
      relayBuzzer(false);
      alertAccidentToFamily("XE DANG DI CHUYEN");
    }
  }

  // nếu xe đứng / đi chậm: gọi ngay khi phát hiện, chờ 30 giây nếu không có D thì gửi cho người thân
  if (accidentTriggered && accidentModeStopped && !accidentConfirmed) {
    relayBuzzer(true);
    if (millis() - stoppedCancelWindowStartMs >= 30000UL) {
      accidentConfirmed = true;
      relayBuzzer(false);
      alertAccidentToFamily("XE DANG DUNG / DI CHAM");
    }
  }
}

// -------------------- INA3221 --------------------
void readIna3221AndPush() {
  if (!ina3221.isConnected()) return;

  const float mainVoltage = ina3221.getBusVoltage(0); // CH1+
  const float auxVoltage  = ina3221.getBusVoltage(1); // CH2+

  const float mainPct = batteryPercentFromVoltage(mainVoltage);
  const float auxPct  = batteryPercentFromVoltage(auxVoltage);

  Blynk.virtualWrite(V5, mainPct);
  Blynk.virtualWrite(V6, auxPct);
  sendBatteryWarnings(mainPct, auxPct);
}

// -------------------- BLYNK --------------------
BLYNK_CONNECTED() {
  Blynk.syncVirtual(V0, V2);
}

BLYNK_WRITE(V2) {
  const int state = param.asInt();
  lockMode = (state == 1);

  if (lockMode) {
    theftMode = true;
    captureTheftBase();
    setStatus("Da khoa vi tri cua xe");
  } else {
    resetTheftMode();
  }
}

BLYNK_WRITE(V0) {
  const int state = param.asInt();
  if (state == 1) {
    resetAccidentMode();
    setStatus("Da dung xe len");
    Blynk.virtualWrite(V0, 0);
  }
}

// -------------------- SETUP / LOOP --------------------
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(RELAY_BUZZER_PIN, OUTPUT);
  relayBuzzer(false);

  if (MPU_INT_PIN >= 0) pinMode(MPU_INT_PIN, INPUT_PULLUP);

  Wire.begin(I2C_SDA, I2C_SCL);

  // MPU6050
  if (!mpu.begin()) {
    Serial.println("MPU6050 not found");
  } else {
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    takeOrientationBaseline();
  }

  // INA3221
  if (!ina3221.begin()) {
    Serial.println("INA3221 not found");
  } else {
    ina3221.setAverage(4);
    ina3221.setBusVoltageConversionTime(4);
    ina3221.setShuntVoltageConversionTime(4);
    ina3221.setModeShuntBusContinuous();
  }

  // GPS
  GpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);

  // Modem A7680C
  ModemSerial.begin(115200, SERIAL_8N1, A7680C_RX, A7680C_TX);
  delay(1200);
  modemSend("AT", 300);
  modemSend("ATE0", 300);
  modemSend("AT+CMGF=1", 300);
  modemSend("AT+CSCS=\"GSM\"", 300);

  // WiFi / Blynk
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Blynk.config(BLYNK_AUTH);
  Blynk.connect(10000);

  // Initial UI
  Blynk.virtualWrite(V0, 0);
  Blynk.virtualWrite(V2, 0);
  Blynk.virtualWrite(V3, "Dang khoi dong...");
  Blynk.virtualWrite(V4, "");
  Blynk.virtualWrite(V5, 0);
  Blynk.virtualWrite(V6, 0);

  // Timers
  timer.setInterval(100L, []() {
    while (GpsSerial.available()) {
      gps.encode(GpsSerial.read());
    }
  });

  timer.setInterval(200L, []() {
    readIna3221AndPush();
  });

  timer.setInterval(500L, []() {
    pollModemForSms();
    handleTheft();
  });

  timer.setInterval(200L, []() {
    sensors_event_t a, g, t;
    if (!mpu.getEvent(&a, &g, &t)) return;

    const float ax = a.acceleration.x / 9.80665f;
    const float ay = a.acceleration.y / 9.80665f;
    const float az = a.acceleration.z / 9.80665f;
    const float gMag = sqrtf(ax * ax + ay * ay + az * az);

    const float rollDeg = rad2deg(atan2f(ay, az));
    const float pitchDeg = rad2deg(atan2f(-ax, sqrtf(ay * ay + az * az)));
    const float tiltDeg = max(fabsf(rollDeg - baselineRollDeg), fabsf(pitchDeg - baselinePitchDeg));

    // V1: hiển thị góc + g
    String v1Text = String("G=") + String(gMag, 2) + "g, Goc=" + String(tiltDeg, 1) + " deg";
    Blynk.virtualWrite(V1, v1Text);

    // deep sleep: góc và g gần như không đổi trong 10 giây
    const bool stableNow = (fabsf(gMag - lastStableG) < 0.03f) && (fabsf(tiltDeg - lastStableTilt) < 1.0f);
    if (stableNow && !theftMode && !accidentTriggered && !accidentConfirmed) {
      if (stableStartMs == 0) stableStartMs = millis();
      if (!deepSleepEnabled && millis() - stableStartMs >= 10000UL) {
        enterDeepSleep();
      }
    } else {
      stableStartMs = 0;
      lastStableG = gMag;
      lastStableTilt = tiltDeg;
    }

    handleAccident(gMag, tiltDeg);
  });
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
  }

  if (!Blynk.connected()) {
    Blynk.connect(1000);
  }

  Blynk.run();
  timer.run();
}
