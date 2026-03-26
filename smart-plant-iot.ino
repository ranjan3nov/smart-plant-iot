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
#define DHTTYPE    DHT22  // Change to DHT11 if your sensor is a DHT11
#define RELAY_PIN  25
#define TRIG_PIN   5   // Ultrasonic Trig
#define ECHO_PIN   18  // Ultrasonic Echo

// -- Default API URL (fallback if no config saved in flash) --
const String DEFAULT_API_URL   = "http://10.52.144.50:8000/api/sensor-data";
const String DEFAULT_CONFIG_URL = "http://10.52.144.50:8000/api/config";

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
    bool usingDefault = (laravel_api_url == DEFAULT_API_URL);
    String urlSource  = usingDefault ? "default (not customised)" : "saved in flash";

    String html =
      "<html><head>"
      "<meta http-equiv='refresh' content='10'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<style>"
        "body{font-family:sans-serif;max-width:520px;margin:2rem auto;padding:0 1rem;color:#e5e7eb;background:#111827;}"
        "h2{font-size:1.1rem;margin:0 0 1.2rem;color:#fff;}"
        "h3{font-size:.75rem;text-transform:uppercase;letter-spacing:.08em;color:#6b7280;margin:1.2rem 0 .5rem;}"
        ".card{background:#1f2937;border:1px solid #374151;border-radius:.75rem;padding:1rem;margin-bottom:.75rem;}"
        ".row{display:flex;justify-content:space-between;align-items:baseline;padding:.3rem 0;border-bottom:1px solid #374151;font-size:.8rem;}"
        ".row:last-child{border-bottom:none;}"
        ".label{color:#9ca3af;}"
        ".value{color:#fff;font-weight:500;text-align:right;word-break:break-all;max-width:65%;}"
        ".value.mono{font-family:monospace;font-size:.75rem;color:#34d399;}"
        ".value.warn{color:#fbbf24;}"
        "input[type=text]{width:100%;box-sizing:border-box;background:#111827;border:1px solid #374151;border-radius:.5rem;padding:.5rem .75rem;color:#fff;font-size:.8rem;margin:.4rem 0;}"
        "button,input[type=submit]{width:100%;padding:.55rem;border:none;border-radius:.5rem;font-size:.8rem;font-weight:600;cursor:pointer;margin-top:.4rem;}"
        ".btn-primary{background:#10b981;color:#fff;}"
        ".btn-secondary{background:#374151;color:#d1d5db;}"
        ".btn-danger{background:#7f1d1d;color:#fca5a5;}"
        ".tag{display:inline-block;font-size:.65rem;padding:.15rem .5rem;border-radius:.3rem;margin-left:.4rem;vertical-align:middle;}"
        ".tag-default{background:#1f2937;border:1px solid #374151;color:#6b7280;}"
        ".tag-custom{background:#064e3b;border:1px solid #065f46;color:#6ee7b7;}"
      "</style>"
      "</head>"
      "<body>"
      "<h2>&#127807; Smart Plant &mdash; Device</h2>";

    // --- Stored Config ---
    html += "<h3>Stored Configuration</h3>";
    html += "<div class='card'>";
    html += "<div class='row'><span class='label'>API URL</span>"
            "<span class='value mono'>" + laravel_api_url +
            "<span class='tag " + String(usingDefault ? "tag-default" : "tag-custom") + "'>"
            + urlSource + "</span></span></div>";
    html += "<div class='row'><span class='label'>Tank empty threshold</span>"
            "<span class='value'>" + String(tankHeightCm) + " cm <small style='color:#6b7280'>(from server)</small></span></div>";
    html += "<div class='row'><span class='label'>Send interval</span>"
            "<span class='value " + String(currentInterval <= 20000 ? "warn" : "") + "'>"
            + String(currentInterval / 1000) + "s "
            "<small style='color:#6b7280'>" + String(currentInterval <= 20000 ? "alert mode" : "normal mode") + "</small></span></div>";
    html += "<div class='row'><span class='label'>Default URL</span>"
            "<span class='value mono'>" + DEFAULT_API_URL + "</span></div>";
    html += "</div>";

    // --- Live Sensors ---
    html += "<h3>Live Sensor Readings</h3>";
    html += "<div class='card'>";
    html += "<div class='row'><span class='label'>Soil moisture (raw ADC)</span><span class='value'>" + String(analogRead(SOIL_PIN)) + "</span></div>";
    html += "<div class='row'><span class='label'>Rain (raw ADC)</span><span class='value'>" + String(analogRead(RAIN_PIN)) + "</span></div>";
    html += "<div class='row'><span class='label'>Water distance</span><span class='value'>" + String(getWaterLevel()) + " cm</span></div>";
    html += "<div class='row'><span class='label'>Temperature</span><span class='value'>" + String(dht.readTemperature()) + " &deg;C</span></div>";
    html += "<div class='row'><span class='label'>Humidity</span><span class='value'>" + String(dht.readHumidity()) + " %</span></div>";
    html += "<div class='row'><span class='label'>Last sync</span><span class='value'>" + lastStatus + "</span></div>";
    html += "</div>";

    // --- Actions ---
    html += "<h3>Actions</h3>";
    html += "<div class='card'>";
    html += "<form action='/update' method='POST'>"
              "<label style='font-size:.75rem;color:#9ca3af'>Change API URL</label>"
              "<input type='text' name='url' value='" + laravel_api_url + "'>"
              "<input type='submit' class='btn-primary' value='Save URL'>"
            "</form>";
    html += "<form action='/reload-config' method='POST' style='margin-top:.6rem'>"
              "<input type='submit' class='btn-secondary' value='Reload Config from Server'>"
            "</form>";
    html += "<form action='/reset' method='POST' style='margin-top:.4rem'>"
              "<input type='submit' class='btn-danger' value='Reset to Default URL'"
              " onclick=\"return confirm('Reset URL to default?')\">"
            "</form>";
    html += "</div>";

    html += "<p style='font-size:.65rem;color:#4b5563;text-align:center'>Auto-refreshes every 10s</p>";
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

    StaticJsonDocument<512> sendDoc;
    sendDoc["moisture"]              = analogRead(SOIL_PIN);
    sendDoc["rain"]                  = analogRead(RAIN_PIN);
    sendDoc["temp"]                  = temp;
    sendDoc["humidity"]              = humidity;
    sendDoc["water_dist"]            = waterDistance;
    sendDoc["tank_status"]           = tankEmpty ? "EMPTY" : "OK";
    sendDoc["device_url"]            = laravel_api_url;
    sendDoc["device_tank_height_cm"] = tankHeightCm;

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
