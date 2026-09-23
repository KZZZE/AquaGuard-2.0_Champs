#define BLYNK_TEMPLATE_ID "TMPL6lRPjq8Ra"
#define BLYNK_TEMPLATE_NAME "AquaGuard Mini"
#define BLYNK_AUTH_TOKEN "ouKXq1OeyBwt6kaym5Dgd-GnMT5ZYEuw"

#define REMOTEXY_MODE__WIFI_CLOUD

#include <WiFi.h>
#include <RemoteXY.h>
#include <BlynkSimpleEsp32.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <time.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>


// =========================
// REMOTEXY
// =========================

#define REMOTEXY_WIFI_SSID "superpower"
#define REMOTEXY_WIFI_PASSWORD "12121212"
#define REMOTEXY_CLOUD_SERVER "cloud.remotexy.com"
#define REMOTEXY_CLOUD_PORT 6376
#define REMOTEXY_CLOUD_TOKEN "1c5251ffa6cdb1877cb9564048f59773"

#pragma pack(push, 1)

uint8_t const PROGMEM RemoteXY_CONF_PROGMEM[] = {
  255,2,0,0,0,22,0,19,0,0,0,0,31,1,106,200,1,1,1,0,
  5,23,72,60,60,0,2,26,31
};

struct {
  int8_t joystick_01_x;
  int8_t joystick_01_y;
  uint8_t connect_flag;
} RemoteXY;

#pragma pack(pop)


// =========================
// WIFI
// =========================

char ssid[] = "superpower";
char pass[] = "12121212";


// =========================
// TELEGRAM 
// =========================

#define TELEGRAM_BOT_TOKEN "8898820256:AAEQ8_zbUYAgwBarl7Yz4vF5wt2wHepJ5qQ"
#define TELEGRAM_CHAT_ID "1330259963"

WiFiClientSecure telegramClient;
UniversalTelegramBot bot(TELEGRAM_BOT_TOKEN, telegramClient);


// =========================
// OLED
// =========================

#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_ADDR 0x3C

Adafruit_SSD1306 display(
  128,
  64,
  &Wire,
  -1
);


// =========================
// SENSOR
// =========================

#define TDS_PIN 34
#define DHT_PIN 4
#define PH_PIN 35
#define DS18B20_PIN 18
#define TOUCH_PIN 15

#define DHT_TYPE DHT11


// =========================
// MOTOR DRIVER
// =========================

#define ENA 25
#define IN1 26
#define IN2 27

#define IN3 32
#define IN4 33

#define ENB 13

#define MOTOR_LEFT_CHANNEL 0
#define MOTOR_RIGHT_CHANNEL 1


// =========================
// OBJECT
// =========================

DHT dht(
  DHT_PIN,
  DHT_TYPE
);

OneWire oneWire(
  DS18B20_PIN
);

DallasTemperature waterSensor(
  &oneWire
);

BlynkTimer timer;


// =========================
// SENSOR VARIABLE
// =========================

float tdsValue = 0;
float airTemperature = 0;
float humidity = 0;
float phValue = 0;
float waterTemperature = 0;


// =========================
// TELEGRAM ALARM
// =========================

bool tdsAlarm = false;
bool waterTempAlarm = false;
bool airTempAlarm = false;
bool phLowAlarm = false;
bool phHighAlarm = false;


// =========================
// OLED PAGE
// =========================

int oledPage = 0;

int lastTouchState = LOW;

unsigned long lastTouchTime = 0;


// =========================
// MOTOR SPEED
// =========================

int MAX_SPEED = 250;


// =========================
// STOP MOTOR
// =========================

void stopMotor() {

  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);

  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);

  ledcWrite(MOTOR_LEFT_CHANNEL, 0);
  ledcWrite(MOTOR_RIGHT_CHANNEL, 0);
}


// =========================
// MOTOR CONTROL
// =========================

void moveMotor(
  int leftMotor,
  int rightMotor
) {

  leftMotor = constrain(
    leftMotor,
    -255,
    255
  );

  rightMotor = constrain(
    rightMotor,
    -255,
    255
  );


  if (leftMotor > 0) {

    digitalWrite(IN1, HIGH);
    digitalWrite(IN2, LOW);

  }

  else if (leftMotor < 0) {

    digitalWrite(IN1, LOW);
    digitalWrite(IN2, HIGH);

  }

  else {

    digitalWrite(IN1, LOW);
    digitalWrite(IN2, LOW);

  }


  if (rightMotor > 0) {

    digitalWrite(IN3, HIGH);
    digitalWrite(IN4, LOW);

  }

  else if (rightMotor < 0) {

    digitalWrite(IN3, LOW);
    digitalWrite(IN4, HIGH);

  }

  else {

    digitalWrite(IN3, LOW);
    digitalWrite(IN4, LOW);

  }


  ledcWrite(
    MOTOR_LEFT_CHANNEL,
    abs(leftMotor)
  );

  ledcWrite(
    MOTOR_RIGHT_CHANNEL,
    abs(rightMotor)
  );
}


// =========================
// ROBOT CONTROL
// =========================

void controlRobot() {

  if (!RemoteXY.connect_flag) {

    stopMotor();

    return;
  }


  int x =
    RemoteXY.joystick_01_x;

  int y =
    RemoteXY.joystick_01_y;


  int leftMotor =
    y + x;

  int rightMotor =
    y - x;


  leftMotor = constrain(
    leftMotor,
    -100,
    100
  );

  rightMotor = constrain(
    rightMotor,
    -100,
    100
  );


  leftMotor = map(
    leftMotor,
    -100,
    100,
    -MAX_SPEED,
    MAX_SPEED
  );

  rightMotor = map(
    rightMotor,
    -100,
    100,
    -MAX_SPEED,
    MAX_SPEED
  );


  if (
    abs(x) < 5 &&
    abs(y) < 5
  ) {

    stopMotor();

  }

  else {

    moveMotor(
      leftMotor,
      rightMotor
    );

  }
}


// =========================
// TELEGRAM ALERT
// =========================

void checkTelegramAlerts() {

  if (
    tdsValue > 600 &&
    !tdsAlarm
  ) {

    String message =
      "PERINGATAN AQUAGUARD\n\n"
      "TDS air terlalu tinggi!\n"
      "TDS: " +
      String(tdsValue, 0) +
      " ppm\n"
      "Batas: 600 ppm";

    bot.sendMessage(
      TELEGRAM_CHAT_ID,
      message,
      ""
    );

    tdsAlarm = true;
  }

  if (tdsValue <= 600) {
    tdsAlarm = false;
  }


  if (
    waterTemperature > 40 &&
    !waterTempAlarm
  ) {

    String message =
      "PERINGATAN AQUAGUARD\n\n"
      "Suhu air terlalu tinggi!\n"
      "Suhu air: " +
      String(waterTemperature, 1) +
      " C\n"
      "Batas: 40 C";

    bot.sendMessage(
      TELEGRAM_CHAT_ID,
      message,
      ""
    );

    waterTempAlarm = true;
  }

  if (waterTemperature <= 40) {
    waterTempAlarm = false;
  }


  if (
    airTemperature > 40 &&
    !airTempAlarm
  ) {

    String message =
      "PERINGATAN AQUAGUARD\n\n"
      "Suhu udara terlalu tinggi!\n"
      "Suhu udara: " +
      String(airTemperature, 1) +
      " C\n"
      "Batas: 40 C";

    bot.sendMessage(
      TELEGRAM_CHAT_ID,
      message,
      ""
    );

    airTempAlarm = true;
  }

  if (airTemperature <= 40) {
    airTempAlarm = false;
  }


  if (
    phValue < 5 &&
    !phLowAlarm
  ) {

    String message =
      "PERINGATAN AQUAGUARD\n\n"
      "pH air terlalu rendah!\n"
      "pH: " +
      String(phValue, 2) +
      "\n"
      "Batas minimum: 5";

    bot.sendMessage(
      TELEGRAM_CHAT_ID,
      message,
      ""
    );

    phLowAlarm = true;
  }

  if (phValue >= 5) {
    phLowAlarm = false;
  }


  if (
    phValue > 9 &&
    !phHighAlarm
  ) {

    String message =
      "PERINGATAN AQUAGUARD\n\n"
      "pH air terlalu tinggi!\n"
      "pH: " +
      String(phValue, 2) +
      "\n"
      "Batas maksimum: 9";

    bot.sendMessage(
      TELEGRAM_CHAT_ID,
      message,
      ""
    );

    phHighAlarm = true;
  }

  if (phValue <= 9) {
    phHighAlarm = false;
  }
}


// =========================
// SENSOR READING
// =========================

void readSensors() {

  float t =
    dht.readTemperature();

  float h =
    dht.readHumidity();


  if (!isnan(t)) {
    airTemperature = t;
  }

  if (!isnan(h)) {
    humidity = h;
  }


  // TDS

  int tdsADC =
    analogRead(TDS_PIN);

  float tdsVoltage =
    tdsADC * 3.3 / 4095.0;

  float coefficient =
    1.0 +
    0.02 *
    (airTemperature - 25.0);

  float compensatedVoltage =
    tdsVoltage /
    coefficient;

  tdsValue =
    (
      133.42 *
      compensatedVoltage *
      compensatedVoltage *
      compensatedVoltage

      -

      255.86 *
      compensatedVoltage *
      compensatedVoltage

      +

      857.39 *
      compensatedVoltage

    ) * 0.5;


  if (tdsValue < 0) {
    tdsValue = 0;
  }


  // pH

  int phADC =
    analogRead(PH_PIN);

  float phVoltage =
    phADC * 3.3 / 4095.0;

  phValue =
    7.0 +
    (
      (2.50 - phVoltage)
      / 0.18
    );

  phValue =
    constrain(
      phValue,
      0,
      14
    );


  // Suhu air

  waterSensor.requestTemperatures();

  float waterTemp =
    waterSensor.getTempCByIndex(0);

  if (
    waterTemp !=
    DEVICE_DISCONNECTED_C
  ) {

    waterTemperature =
      waterTemp;
  }


  // Serial monitor

  Serial.println();
  Serial.println(
    "======================"
  );

  Serial.print("TDS: ");
  Serial.print(tdsValue, 0);
  Serial.println(" ppm");

  Serial.print("Suhu udara: ");
  Serial.print(airTemperature, 1);
  Serial.println(" C");

  Serial.print("Kelembapan: ");
  Serial.print(humidity, 1);
  Serial.println(" %");

  Serial.print("pH: ");
  Serial.println(phValue, 2);

  Serial.print("Suhu air: ");
  Serial.print(waterTemperature, 1);
  Serial.println(" C");


  checkTelegramAlerts();

  updateOLED();
}


// =========================
// SEND TO BLYNK
// =========================

void sendToBlynk() {

  Blynk.virtualWrite(
    V0,
    tdsValue
  );

  Blynk.virtualWrite(
    V1,
    airTemperature
  );

  Blynk.virtualWrite(
    V2,
    humidity
  );

  Blynk.virtualWrite(
    V3,
    phValue
  );

  Blynk.virtualWrite(
    V4,
    waterTemperature
  );


  Serial.println();
  Serial.println(
    "----- BLYNK -----"
  );

  Serial.print("TDS: ");
  Serial.print(tdsValue, 0);
  Serial.println(" ppm");

  Serial.print("Suhu: ");
  Serial.print(airTemperature, 1);
  Serial.println(" C");

  Serial.print("Humidity: ");
  Serial.print(humidity, 1);
  Serial.println(" %");

  Serial.print("pH: ");
  Serial.println(phValue, 2);

  Serial.print("Suhu air: ");
  Serial.print(waterTemperature, 1);
  Serial.println(" C");
}


// =========================
// OLED
// =========================

void updateOLED() {

  display.clearDisplay();

  display.setTextColor(
    SSD1306_WHITE
  );

  display.setTextSize(1);


  display.setCursor(
    0,
    0
  );

  display.print(
    "AQUAGUARD"
  );


  struct tm timeinfo;

  if (
    getLocalTime(
      &timeinfo
    )
  ) {

    char timeString[6];

    strftime(
      timeString,
      sizeof(timeString),
      "%H:%M",
      &timeinfo
    );

    display.setCursor(
      91,
      0
    );

    display.print(
      timeString
    );
  }


  display.drawLine(
    0,
    10,
    127,
    10,
    SSD1306_WHITE
  );


  if (oledPage == 0) {

    display.setTextSize(1);

    display.setCursor(
      48,
      18
    );

    display.print("TDS");

    display.setTextSize(3);

    display.setCursor(
      25,
      32
    );

    display.print(
      tdsValue,
      0
    );

    display.setTextSize(1);

    display.setCursor(
      91,
      45
    );

    display.print("ppm");
  }


  else if (oledPage == 1) {

    display.setTextSize(1);

    display.setCursor(
      35,
      18
    );

    display.print(
      "SUHU UDARA"
    );

    display.setTextSize(3);

    display.setCursor(
      25,
      32
    );

    display.print(
      airTemperature,
      1
    );

    display.setTextSize(1);

    display.setCursor(
      105,
      45
    );

    display.print("C");
  }


  else if (oledPage == 2) {

    display.setTextSize(1);

    display.setCursor(
      30,
      18
    );

    display.print(
      "KELEMBAPAN"
    );

    display.setTextSize(3);

    display.setCursor(
      25,
      32
    );

    display.print(
      humidity,
      0
    );

    display.setTextSize(2);

    display.setCursor(
      95,
      37
    );

    display.print("%");
  }


  else if (oledPage == 3) {

    display.setTextSize(1);

    display.setCursor(
      48,
      18
    );

    display.print(
      "pH AIR"
    );

    display.setTextSize(3);

    display.setCursor(
      30,
      32
    );

    display.print(
      phValue,
      2
    );
  }


  else {

    display.setTextSize(1);

    display.setCursor(
      35,
      18
    );

    display.print(
      "SUHU AIR"
    );

    display.setTextSize(3);

    display.setCursor(
      25,
      32
    );

    display.print(
      waterTemperature,
      1
    );

    display.setTextSize(1);

    display.setCursor(
      105,
      45
    );

    display.print("C");
  }


  display.display();
}


// =========================
// TOUCH
// =========================

void checkTouch() {

  int state =
    digitalRead(
      TOUCH_PIN
    );


  if (
    state == HIGH &&
    lastTouchState == LOW
  ) {

    if (
      millis() -
      lastTouchTime >
      400
    ) {

      oledPage++;


      if (oledPage > 4) {
        oledPage = 0;
      }


      updateOLED();


      lastTouchTime =
        millis();
    }
  }


  lastTouchState =
    state;
}


// =========================
// SETUP
// =========================

void setup() {

  Serial.begin(115200);


  // OLED

  Wire.begin(
    OLED_SDA,
    OLED_SCL
  );


  if (
    !display.begin(
      SSD1306_SWITCHCAPVCC,
      OLED_ADDR
    )
  ) {

    Serial.println(
      "OLED gagal"
    );

    while (true);
  }


  // Sensor

  dht.begin();

  waterSensor.begin();


  pinMode(
    TOUCH_PIN,
    INPUT
  );


  // Motor

  pinMode(
    IN1,
    OUTPUT
  );

  pinMode(
    IN2,
    OUTPUT
  );

  pinMode(
    IN3,
    OUTPUT
  );

  pinMode(
    IN4,
    OUTPUT
  );


  // PWM untuk ESP32 Core 2.0.17

  ledcSetup(
    MOTOR_LEFT_CHANNEL,
    5000,
    8
  );

  ledcSetup(
    MOTOR_RIGHT_CHANNEL,
    5000,
    8
  );

  ledcAttachPin(
    ENA,
    MOTOR_LEFT_CHANNEL
  );

  ledcAttachPin(
    ENB,
    MOTOR_RIGHT_CHANNEL
  );


  stopMotor();


  // ADC

  analogReadResolution(12);

  analogSetPinAttenuation(
    TDS_PIN,
    ADC_11db
  );

  analogSetPinAttenuation(
    PH_PIN,
    ADC_11db
  );


  // OLED startup

  display.clearDisplay();

  display.setTextColor(
    SSD1306_WHITE
  );

  display.setTextSize(2);

  display.setCursor(
    10,
    5
  );

  display.println(
    "AQUA"
  );

  display.setCursor(
    10,
    30
  );

  display.println(
    "GUARD"
  );

  display.display();

  delay(1500);


  // RemoteXY

  RemoteXY_Init();


  // Telegram

  telegramClient.setInsecure();

  bot.sendMessage(
    TELEGRAM_CHAT_ID,
    "AquaGuard online!\n\n"
    "Sistem monitoring Telegram aktif.",
    ""
  );


  // Blynk

  Blynk.config(
    BLYNK_AUTH_TOKEN
  );

  Blynk.connect(
    3000
  );


  // Waktu

  configTime(
    7 * 3600,
    0,
    "pool.ntp.org"
  );


  // Timer

  timer.setInterval(
    2000L,
    readSensors
  );

  timer.setInterval(
    2000L,
    sendToBlynk
  );


  updateOLED();
}


// =========================
// LOOP
// =========================

void loop() {

  RemoteXYEngine.handler();

  Blynk.run();

  timer.run();

  checkTouch();

  controlRobot();
}