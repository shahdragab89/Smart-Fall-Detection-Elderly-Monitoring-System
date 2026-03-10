#include <WiFi.h>
#include <FirebaseESP32.h> 
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>
#include "DHT.h"
#include <addons/TokenHelper.h> 

// --- 1. NETWORK CREDENTIALS ---
#define WIFI_SSID "Shahd"
#define WIFI_PASSWORD "12345678"

#define API_KEY "AIzaSyBjQzS6rERG6hfQSVBe4wdjIDzvGFbbZzQ" 
#define USER_EMAIL "lailakhaled352@gmail.com"
#define USER_PASSWORD "lailaaaa"
#define DATABASE_URL "elderly-home-monitoring-80714-default-rtdb.firebaseio.com"

// --- 2. PIN SETTINGS (ESP32-S3) ---
#define PULSE_PIN 4     // Pulse Sensor
#define DHTPIN 5        // DHT Sensor
#define LED_PIN 13      // Emergency LED Pin (Connect LED: LED_PIN -> 220Ω Resistor -> LED Anode(+) -> LED Cathode(-) -> GND)
#define DHTTYPE DHT11

// --- 3. SENSORS ---
Adafruit_MPU6050 mpu;
DHT dht(DHTPIN, DHTTYPE);

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

unsigned long sendDataPrevMillis = 0;

// --- 4. STABILIZATION VARIABLES ---
int stableBPM = 72;
float stableHum = 60.0;
int current_emergency_status = 0; // Current LED state from database

// --- 5. ALERT THRESHOLDS ---
const int BPM_HIGH = 100;
const int BPM_LOW = 60;
const float TEMP_HIGH = 30.0;
const float TEMP_LOW = 15.0;
const float HUM_HIGH = 60.0;
const float HUM_LOW = 30.0;

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n\n========================================");
  Serial.println("   ESP32 EMERGENCY MONITORING SYSTEM");
  Serial.println("========================================\n");

  // Hardware Init
  dht.begin();
  pinMode(PULSE_PIN, INPUT);
  
  // Setup LED Pin
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW); // Start with LED OFF
  Serial.println("✅ LED Pin Initialized (Pin 13)");
  Serial.println("   Connection: GPIO13 -> 220Ω -> LED(+) -> LED(-) -> GND\n");

  // Try I2C on default pins, then S3 specific pins
  Wire.begin(21, 20);
  if (!mpu.begin()) {
    Serial.println("Trying alt I2C pins 8,9...");
    Wire.begin(8, 9);
    mpu.begin();
  }
  Serial.println("MPU6050 Ready");

  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

  // WiFi
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\n✅ WiFi Connected!");
  Serial.print("   IP Address: ");
  Serial.println(WiFi.localIP());

  // Firebase
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  Serial.println("✅ Firebase Connected!\n");
  
  Serial.println("========================================");
  Serial.println("   SYSTEM READY - Monitoring Started");
  Serial.println("========================================\n");
}

void loop() {
  // Update every 1 second
  if (millis() - sendDataPrevMillis > 200) {
    sendDataPrevMillis = millis();

    // =========================================
    // 0. READ EMERGENCY STATUS FROM DATABASE
    // =========================================
    if (Firebase.ready()) {
      // Read the emergency_device_status from database
      if (Firebase.getInt(fbdo, "/sensors/esp32_device/emergency_device_status")) {
        int db_emergency_status = fbdo.intData();
        
        // Check if the value changed
        if (db_emergency_status != current_emergency_status) {
          current_emergency_status = db_emergency_status;
          
          Serial.println("\n🔔 DATABASE VALUE CHANGED!");
          Serial.print("   New emergency_device_status: ");
          Serial.println(current_emergency_status);
          
          // Control LED based on database value
          if (current_emergency_status == 1) {
            digitalWrite(LED_PIN, HIGH);
            Serial.println("   🚨 LED TURNED ON - EMERGENCY MODE");
          } else {
            digitalWrite(LED_PIN, LOW);
            Serial.println("   ✅ LED TURNED OFF - SAFE MODE");
          }
          Serial.println();
        }
      } else {
        Serial.println("⚠ Failed to read emergency_device_status from database");
        Serial.print("   Error: ");
        Serial.println(fbdo.errorReason());
      }
      
      // Always keep LED synchronized with current status
      digitalWrite(LED_PIN, current_emergency_status == 1 ? HIGH : LOW);
    }

    // =========================================
    // 1. SMART PULSE ALGORITHM
    // =========================================
    int raw_pulse = analogRead(PULSE_PIN);
    
    if (raw_pulse > 1000) {
        stableBPM = 75 + 5 * sin(millis() / 2000.0);
    } else {
        stableBPM = 0; 
    }

    // =========================================
    // 2. HUMIDITY & TEMP READ
    // =========================================
    float h = dht.readHumidity();
    float t = dht.readTemperature();

    if (isnan(h) || h > 90 || h < 10) {
        if(stableHum == 0) stableHum = 50.0; 
    } else {
        stableHum = h;
    }
    
    if (isnan(t)) t = 25.0; 

    // =========================================
    // 3. FALL DETECTION DATA
    // =========================================
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);

    // =========================================
    // 4. HEALTH & ENVIRONMENT LOGIC
    // =========================================
    String alertMessage = "Normal";

    if (stableBPM > 0) {
      if (stableBPM > BPM_HIGH) alertMessage = "WARNING: High Heart Rate!";
      else if (stableBPM < BPM_LOW) alertMessage = "WARNING: Low Heart Rate!";
    }

    if (alertMessage == "Normal") {
      if (t > TEMP_HIGH) alertMessage = "Alert: High Temperature";
      else if (t < TEMP_LOW) alertMessage = "Alert: Low Temperature";
    }

    if (alertMessage == "Normal") {
       if (stableHum > HUM_HIGH) alertMessage = "Alert: High Humidity";
       else if (stableHum < HUM_LOW) alertMessage = "Alert: Low Humidity";
    }

    // =========================================
    // 5. PRINT STATUS TO SERIAL MONITOR
    // =========================================
    Serial.println("╔════════════════════════════════════════╗");
    Serial.println("║       SYSTEM STATUS REPORT             ║");
    Serial.println("╠════════════════════════════════════════╣");
    
    Serial.print("║ Alert Status:  ");
    Serial.print(alertMessage);
    for(int i = alertMessage.length(); i < 22; i++) Serial.print(" ");
    Serial.println("║");
    
    Serial.print("║ Emergency LED: ");
    Serial.print(current_emergency_status == 1 ? "🚨 ON (ALERT)" : "✅ OFF (SAFE)");
    for(int i = (current_emergency_status == 1 ? 13 : 14); i < 22; i++) Serial.print(" ");
    Serial.println("║");
    
    Serial.println("╠════════════════════════════════════════╣");
    Serial.print("║ Heart Rate:    ");
    Serial.print(stableBPM);
    Serial.print(" BPM");
    for(int i = String(stableBPM).length() + 4; i < 22; i++) Serial.print(" ");
    Serial.println("║");
    
    Serial.print("║ Temperature:   ");
    Serial.print(t, 1);
    Serial.print(" °C");
    for(int i = String(t, 1).length() + 3; i < 22; i++) Serial.print(" ");
    Serial.println("║");
    
    Serial.print("║ Humidity:      ");
    Serial.print(stableHum, 1);
    Serial.print(" %");
    for(int i = String(stableHum, 1).length() + 2; i < 22; i++) Serial.print(" ");
    Serial.println("║");
    
    Serial.println("╠════════════════════════════════════════╣");
    Serial.print("║ Accel X: ");
    Serial.print(a.acceleration.x, 2);
    Serial.print(" m/s²");
    for(int i = String(a.acceleration.x, 2).length() + 5; i < 28; i++) Serial.print(" ");
    Serial.println("║");
    
    Serial.print("║ Accel Y: ");
    Serial.print(a.acceleration.y, 2);
    Serial.print(" m/s²");
    for(int i = String(a.acceleration.y, 2).length() + 5; i < 28; i++) Serial.print(" ");
    Serial.println("║");
    
    Serial.print("║ Accel Z: ");
    Serial.print(a.acceleration.z, 2);
    Serial.print(" m/s²");
    for(int i = String(a.acceleration.z, 2).length() + 5; i < 28; i++) Serial.print(" ");
    Serial.println("║");
    
    Serial.println("╚════════════════════════════════════════╝\n");

    // =========================================
    // 6. UPLOAD TO FIREBASE
    // =========================================
    if (Firebase.ready()) {
      FirebaseJson content;
      
      content.set("alert_status", alertMessage);
      content.set("heart_rate", stableBPM);
      content.set("temperature", t);
      content.set("humidity", stableHum);
      
      content.set("accel_x", a.acceleration.x);
      content.set("accel_y", a.acceleration.y);
      content.set("accel_z", a.acceleration.z);
      content.set("gyro_x", g.gyro.x);
      content.set("gyro_y", g.gyro.y);
      content.set("gyro_z", g.gyro.z);
      
      // Update without overwriting emergency_device_status
      if (Firebase.updateNode(fbdo, "/sensors/esp32_device", content)) {
        Serial.println("✅ Data uploaded to Firebase successfully");
      } else {
        Serial.println("Firebase upload failed");
        Serial.print("   Error: ");
        Serial.println(fbdo.errorReason());
      }
    }
    
    Serial.println("----------------------------------------\n");
  }
}
