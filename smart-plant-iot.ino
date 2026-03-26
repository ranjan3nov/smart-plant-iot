#include <WiFiManager.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <DHT.h>

// -- Pin Definitions --
#define SOIL_PIN   34
#define RAIN_PIN   32
#define DHTPIN     4
#define DHTTYPE    DHT11
#define RELAY_PIN  25
#define TRIG_PIN   5   // Ultrasonic Trig
#define ECHO_PIN   18  // Ultrasonic Echo

// -- Default API URL (fallback if no config saved in flash) --
const String DEFAULT_API_URL   = "http://smart-farm.test/api/sensor-data";
const String DEFAULT_CONFIG_URL = "http://smart-farm.test/api/config";

DHT dht(DHTPIN, DHTTYPE);
WebServer server(80);
String laravel_api_url = DEFAULT_API_URL;
String lastStatus = "Waiting for first sync...";
long currentInterval = 300000; // ms — updated by server each cycle
float tankHeightCm   = 20.0;  // cm — loaded from /api/config on boot

// --- Ultrasonic: returns distance in cm ---
float getWaterLevel() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 30000); // 30ms timeout
  return duration * 0.034 / 2;
}

// Derive config URL from the sensor URL (replace "/sensor-data" with "/config")
String configUrl() {
  String url = laravel_api_url;
  int pos = url.lastIndexOf("/sensor-data");
  if (pos >= 0) { url = url.substring(0, pos) + "/config"; }
  return url;
}

// Fetch device config from server once on boot; persists in LittleFS
void fetchConfig() {
  HTTPClient http;
  http.setTimeout(5000);
  http.begin(configUrl());
  int code = http.GET();

  if (code == 200) {
    StaticJsonDocument<128> doc;
    DeserializationError err = deserializeJson(doc, http.getString());
    if (!err) {
      tankHeightCm = doc["tank_height_cm"] | 20.0;
      Serial.println("Config loaded — tank_height_cm: " + String(tankHeightCm) + "cm");

      // Persist to flash so the value survives reboots without network
      File f = LittleFS.open("/device_config.txt", "w");
      f.print(tankHeightCm);
      f.close();
    }
  } else {
    Serial.println("Config fetch failed (" + String(code) + ") — trying flash");
    if (LittleFS.exists("/device_config.txt")) {
      File f = LittleFS.open("/device_config.txt", "r");
      tankHeightCm = f.readString().toFloat();
      f.close();
      if (tankHeightCm <= 0) { tankHeightCm = 20.0; }
      Serial.println("Config from flash — tank_height_cm: " + String(tankHeightCm) + "cm");
    }
  }

  http.end();
}

void setup() {
  Serial.begin(115200);
  dht.begin();
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(RELAY_PIN, LOW);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  }

  // Load saved API URL from flash; fall back to DEFAULT_API_URL if missing or empty
  if (LittleFS.exists("/config.txt")) {
    File file = LittleFS.open("/config.txt", "r");
    String saved = file.readString();
    file.close();
    if (saved.length() > 0) {
      laravel_api_url = saved;
      Serial.println("Loaded URL from flash: " + laravel_api_url);
    } else {
      Serial.println("Empty config — using default URL");
    }
  } else {
    Serial.println("No config found — using default URL: " + laravel_api_url);
  }

  WiFiManager wm;
  if (!wm.autoConnect("Smart-Plant")) ESP.restart();

  Serial.println("WiFi connected. API URL: " + laravel_api_url);
  fetchConfig();

  // --- Debug UI ---
  server.on("/", []() {
    String html = "<html><head><meta http-equiv='refresh' content='5'></head>"
                  "<body style='font-family:sans-serif;text-align:center;max-width:480px;margin:auto;'>";
    html += "<h2>Smart Plant &mdash; Debug</h2>";
    html += "<p><b>Soil (raw):</b> "  + String(analogRead(SOIL_PIN))  + "</p>";
    html += "<p><b>Rain (raw):</b> "  + String(analogRead(RAIN_PIN))  + "</p>";
    html += "<p><b>Water dist:</b> "  + String(getWaterLevel())       + " cm</p>";
    html += "<p><b>Temp:</b> "        + String(dht.readTemperature()) + " &deg;C</p>";
    html += "<p><b>Humidity:</b> "    + String(dht.readHumidity())    + " %</p>";
    html += "<p><b>Last sync:</b> "   + lastStatus                    + "</p>";
    html += "<p><b>Send interval:</b> " + String(currentInterval / 1000) + "s (20=alert, 300=normal)</p>";
    html += "<p><b>Tank threshold:</b> " + String(tankHeightCm) + "cm (from server)</p>";
    html += "<hr>";
    html += "<form action='/update' method='POST'>"
              "API URL:<br>"
              "<input name='url' style='width:90%' value='" + laravel_api_url + "'><br><br>"
              "<input type='submit' value='Save URL'>"
            "</form>";
    html += "<br><form action='/reload-config' method='POST'>"
              "<input type='submit' value='Reload Config from Server'>"
            "</form>";
    html += "<br><form action='/reset' method='POST'>"
              "<input type='submit' value='Reset to Default URL' style='color:red'"
              " onclick=\"return confirm('Reset URL to default?')\">"
            "</form>";
    html += "</body></html>";
    server.send(200, "text/html", html);
  });

  // Save a new URL to flash
  server.on("/update", HTTP_POST, []() {
    String newUrl = server.arg("url");
    if (newUrl.length() == 0) {
      server.send(400, "text/plain", "URL cannot be empty.");
      return;
    }
    laravel_api_url = newUrl;
    File file = LittleFS.open("/config.txt", "w");
    file.print(laravel_api_url);
    file.close();
    Serial.println("URL updated to: " + laravel_api_url);
    server.send(200, "text/plain", "URL saved: " + laravel_api_url);
  });

  // Re-fetch config from server without rebooting
  server.on("/reload-config", HTTP_POST, []() {
    fetchConfig();
    server.send(200, "text/plain", "Config reloaded — tank_height_cm: " + String(tankHeightCm) + "cm");
  });

  // Reset URL back to the hardcoded default
  server.on("/reset", HTTP_POST, []() {
    laravel_api_url = DEFAULT_API_URL;
    LittleFS.remove("/config.txt");
    Serial.println("URL reset to default: " + laravel_api_url);
    server.send(200, "text/plain", "Reset to default: " + laravel_api_url);
  });

  server.begin();
}

void loop() {
  server.handleClient();

  static unsigned long lastTime = 0;
  // Interval is set dynamically by the server:
  //   20s  — alert mode (soil dry, pump running, or tank empty)
  //   300s — normal mode (plant is healthy)
  if (millis() - lastTime > (unsigned long)currentInterval) {
    lastTime = millis();

    float temp          = dht.readTemperature();
    float humidity      = dht.readHumidity();
    float waterDistance = getWaterLevel();

    // Validate DHT reading before sending
    if (isnan(temp) || isnan(humidity)) {
      lastStatus = "DHT read failed — skipping";
      Serial.println(lastStatus);
      return;
    }

    // SAFETY: tank empty if sensor timed out (0) or distance exceeds threshold
    bool tankEmpty = (waterDistance <= 0 || waterDistance > tankHeightCm);

    HTTPClient http;
    http.setTimeout(5000);
    http.begin(laravel_api_url);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<300> sendDoc;
    sendDoc["moisture"]    = analogRead(SOIL_PIN);
    sendDoc["rain"]        = analogRead(RAIN_PIN);
    sendDoc["temp"]        = temp;
    sendDoc["humidity"]    = humidity;
    sendDoc["water_dist"]  = waterDistance;
    sendDoc["tank_status"] = tankEmpty ? "EMPTY" : "OK";

    String jsonString;
    serializeJson(sendDoc, jsonString);

    Serial.println("POST -> " + laravel_api_url);
    Serial.println(jsonString);

    int httpCode = http.POST(jsonString);

    if (httpCode > 0) {
      String response = http.getString();
      Serial.println("Response (" + String(httpCode) + "): " + response);

      StaticJsonDocument<200> recvDoc;
      DeserializationError err = deserializeJson(recvDoc, response);

      if (!err) {
        // Only turn ON if server says so AND tank is NOT empty
        if (recvDoc["pump"] == "ON" && !tankEmpty) {
          digitalWrite(RELAY_PIN, HIGH);
          lastStatus = "Pump ON (at " + String(millis() / 1000) + "s)";
        } else {
          digitalWrite(RELAY_PIN, LOW);
          lastStatus = "Pump OFF (at " + String(millis() / 1000) + "s)";
        }

        // Server tells us how long to wait before the next send
        long serverInterval = recvDoc["next_interval"] | 300;
        currentInterval = serverInterval * 1000L;
        Serial.println("Next interval: " + String(serverInterval) + "s");
      } else {
        digitalWrite(RELAY_PIN, LOW); // Safe default on parse failure
        lastStatus = "JSON parse error — pump OFF";
        Serial.println("JSON error: " + String(err.c_str()));
      }
    } else {
      digitalWrite(RELAY_PIN, LOW); // Safe default on HTTP failure
      lastStatus = "HTTP error: " + String(httpCode);
      Serial.println(lastStatus);
    }

    http.end();
  }
}
