#include <WiFiManager.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <DHT.h>

// -- PIN Defination --
#define SOIL_PIN 34
#define RAIN_PIN 32
#define DHTPIN 4
#define DHTTYPE DHT11
#define RELAY_PIN 25
#define TRIG_PIN 5 // Ultrasonic Trig
#define ECHO_PIN 18 // Utlrasonic Echo

DHT dht(DHTPIN, DHTTYPE);
WebServer server(80);
String laravel_api_url = "http://192.168.x.x:8000/api/sensor-data";
String lastStatus = "Waiting for first sync...";

// --- ULTRASONIC SENSOR FUNCTION ---
float getWaterLevel(){
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 30000); // 30ms timeout
  return duration * 0.034/2; // Returns distance in cm
}

void setup(){
  Serial.begin(115200);
  dht.begin();
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(RELAY_PIN, LOW);

  if(!LittleFS.begin(true)) Serial.println("LittleFS Error");

  // Load saved Laravel URL from the internal memory
  if(LittleFS.exists("/config.txt")){
    File file = LittleFS.open("/config.txt","r");
    laravel_api_url = file.readString();
    file.close();
  }

  WiFiManager wm;
  if (!wm.autoConnect("Smart-Plan")) ESP.restart();

  // --- DEBUG UI ---
  server.on("/", [](){
    String html="<html><head><meta http-equiv='refresh' content='5'></head><body style='font-family:sans-serif; text-align:center;'>";
    html += "<h1>Plant Hardware Debug</h1>";
    html += "<p><b>Moisture:</b> "+ String(analogRead(SOIL_PIN)) + "</p>";
    html += "<p><b>Rain:</b> "+ String(analogRead(RAIN_PIN))+ "</p>";
    html += "<p><b>Water Distance:</b> "+ String(getWaterLevel()) + " cm</p>";
    html += "<p><b>Last Sync:</b> " + lastStatus + "</p>";
    html += "<hr>";
    html += "<form action='/update' method='POST'>New API URL:<br><input name='url' style='width:80%' value='"+laravel_api_url+"'><br><input type='submit' value='Update URL'></form>";
    html += "</body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/update", HTTP_POST, [](){
    laravel_api_url = server.arg("url");
    File file = LittleFS.open("/config.txt", "w");
    file.print(laravel_api_url);
    file.close();
    server.send(200, "text/plain", "URL Updated! Device will use new IP on next sync.");
  });

  server.begin();
}

void loop(){
  server.handleClient();

  static unsigned long lastTime = 0;
  const long interval = 20000; // 20 seconds wait

  if (millis() - lastTime > interval) {
    lastTime = millis(); // Reset timer immediately to perevent overlap

    float waterDistance = getWaterLevel();

    // --- CRITICAL SAFETY LOGIC ---
    // if distance > 20 cm (tank empty), force pump OFF no matter what
    bool tankEmpty = (waterDistance > 20.0 || waterDistance <= 0);

    HTTPClient http;
    http.setTimeout(5000); // Wait max 5s for Laravel to answer
    http.begin(laravel_api_url);
    http.addHeader("Content-Type","application/json");

    StaticJsonDocument<300> sendDoc;
    sendDoc["moisture"] = analogRead(SOIL_PIN);
    sendDoc["rain"] = analogRead(RAIN_PIN);
    sendDoc["temp"] = dht.readTemperature();
    sendDoc["humidity"] = dht.readHumidity();
    sendDoc["water_dist"] = waterDistance;
    sendDoc["tank_status"] = tankEmpty ? "EMPTY" : "OK";

    String jsonString;
    serializeJson(sendDoc, jsonString);
    int httpCode = http.POST(jsonString);

    if(httpCode > 0 ){
      String response = http.getString();
      StaticJsonDocument<200> recvDoc;
      deserializeJson(recvDoc, response);

      // Only turn ON if Laravel says so AND tanks is NOT empty
      if (recvDoc["pump"] == "ON" && !tankEmpty){
        digitalWrite(RELAY_PIN, HIGH);
        lastStatus = "Pump ON (at " + String(millis()/1000) + "s)";
      } else {
        digitalWrite(RELAY_PIN, LOW);
        lastStatus = "Pump OFF (at " + String(millis()/1000) + "s)";
      }
    }
    else {
      lastStatus = "Error: " + String(httpCode);
    }
    http.end();
  }
}