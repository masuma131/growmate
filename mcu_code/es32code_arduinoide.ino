#include <WiFi.h>
#include <HTTPClient.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#define FIREBASE_SECRET "FIREBASE SECRET"

// Set this manually based on season
// -14400 = UTC -4 (EDT), -18000 = UTC -5 (EST)
#define TIMEZONE_OFFSET -14400  // change to -18000 when DST ends

const char *ssid = "Ihitilan";
const char *password = "Medhurst78";
const char *firebase_url = "FIREBASE_URL";
const char *predict_url = "https://us-central1-growmate-455421.cloudfunctions.net/predict_watering";

WiFiServer server(80);
WiFiUDP ntpUDP;

// Directly use EST NTP server
NTPClient timeClient(ntpUDP, "time.nist.gov", TIMEZONE_OFFSET, 60000);

#define RX2_PIN 16  
#define TX2_PIN 17  
#define BAUD_RATE 9600

const int fanPin = 12;
const int lightPin = 14;

bool autoWateringEnabled = true; // Controls whether watering logic runs automatically

unsigned long lastDataUpdate = 0;  //Track last update
String lastTimestamp = "";

// State machine variables
enum SystemState {
  IDLE,
  WATERING,
  WAITING_FOR_FINAL_READING,
};

SystemState currentState = IDLE;
unsigned long stateStartTime = 0;
unsigned long wateringEndTime = 0;

// Store readings
float temperature = 0;
float humidity = 0; 
float moisture = 0;
float light = 0;
float moisture_after = -1; // -1 indicates not read yet
float predictedTime = 0;

void setup() {
    Serial.begin(9600);
    Serial2.begin(BAUD_RATE, SERIAL_8N1, RX2_PIN, TX2_PIN);
    Serial.println("\nESP32 is ready to receive data from K66F...");

    pinMode(2, OUTPUT);
    pinMode(fanPin, OUTPUT);
    pinMode(lightPin, OUTPUT);

    Serial.print("Connecting to WiFi: ");
    Serial.println(ssid);
    WiFi.begin(ssid, password);

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }

    Serial.println("\nWiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    timeClient.begin();
    timeClient.update();

    server.begin();
}

// Get formatted EST timestamp using localtime (adjusted by NTPClient)
String getFormattedTimeEST() {
  timeClient.update();
  time_t epochTime = timeClient.getEpochTime();  // already adjusted for EST from NTPClient
  struct tm *ptm = localtime(&epochTime);        // use localtime instead of gmtime

  char buffer[30];
  sprintf(buffer, "%04d-%02d-%02d %02d:%02d:%02d",
          ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday,
          ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
  return String(buffer);
}

float extractValue(String json, String key) {
  int idx = json.indexOf("\"" + key + "\":");
  if (idx == -1) return 0;
  int start = idx + key.length() + 3;
  int end = json.indexOf(",", start);
  if (end == -1) end = json.indexOf("}", start);
  return json.substring(start, end).toFloat();
}

float callAIPrediction(float temp, float humidity, float light, float moisture_before) {
    HTTPClient http;
    http.begin(predict_url);
    http.addHeader("Content-Type", "application/json");

    String jsonPayload = "{";
    jsonPayload += "\"temperature\": " + String(temp) + ", ";
    jsonPayload += "\"humidity\": " + String(humidity) + ", ";
    jsonPayload += "\"light\": " + String(light) + ", ";
    jsonPayload += "\"moisture_before\": " + String(moisture_before);
    jsonPayload += "}";

    int httpCode = http.POST(jsonPayload);
    
    float duration = 0;
    if (httpCode == 200) {
      String response = http.getString();
      int idx = response.indexOf(":");
      int end = response.indexOf("}");
      duration = response.substring(idx + 1, end).toFloat();
    }
    http.end();
    return duration;
}

void logToFirebase(String path, String jsonData, bool patch = false) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected. Cannot log to Firebase.");
    return;
  }
  
  HTTPClient http;
  String full_url = String(firebase_url) + path + ".json?auth=" + FIREBASE_SECRET;
  http.begin(full_url);
  http.addHeader("Content-Type", "application/json");
  int code = patch ? http.PATCH(jsonData) : http.PUT(jsonData);
  Serial.println(code == 200 ? "Logged to Firebase!" : "Firebase log error: " + String(code));
  http.end();
}

void handleClient() {
  WiFiClient client = server.available();

  if (client) {
    unsigned long clientTimeout = millis() + 5000;
    String currentLine = "";
    String request = "";

    while (client.connected() && millis() < clientTimeout) {
      if (client.available()) {
        char c = client.read();
        request += c;

        if (c == '\n' && currentLine.length() == 0) {
          String requestLine = request.substring(0, request.indexOf('\r'));

          // Handle toggles with redirects
          if (requestLine.startsWith("GET /toggleAutoWatering")) {
            autoWateringEnabled = !autoWateringEnabled;
            client.println("HTTP/1.1 303 See Other");
            client.println("Location: /");
            client.println();
            return;
          }

          if (requestLine.startsWith("GET /toggleFan")) {
            digitalWrite(fanPin, !digitalRead(fanPin));
            Serial2.print(digitalRead(fanPin) ? "{\"fan\": \"on\"}\n" : "{\"fan\": \"off\"}\n");
            client.println("HTTP/1.1 303 See Other");
            client.println("Location: /");
            client.println();
            return;
          }

          if (requestLine.startsWith("GET /toggleLight")) {
            digitalWrite(lightPin, !digitalRead(lightPin));
            Serial2.print(digitalRead(lightPin) ? "{\"light\": \"on\"}\n" : "{\"light\": \"off\"}\n");
            client.println("HTTP/1.1 303 See Other");
            client.println("Location: /");
            client.println();
            return;
          }

          // Main dashboard page
          client.println("HTTP/1.1 200 OK");
          client.println("Content-type:text/html\r\n");
          client.println("<!DOCTYPE html><html><head><title>GrowMate Dashboard</title><meta http-equiv='refresh' content='5'>");
          client.println("<style>");
          client.println("body { font-family: sans-serif; background: #eef2f3; padding: 15px; }");
          client.println("h2 { color: #2E8B57; }");
          client.println(".section { background: white; border-radius: 10px; padding: 10px; box-shadow: 0 0 10px rgba(0,0,0,0.1); margin-bottom: 15px; }");
          client.println("table { width: 100%; border-collapse: collapse; margin-top: 10px; }");
          client.println("th, td { padding: 10px; text-align: center; border: 1px solid #ccc; }");
          client.println(".toggle { display: flex; align-items: center; justify-content: space-between; margin: 10px 0; }");
          client.println(".toggle a { padding: 10px 20px; border-radius: 30px; background: #4CAF50; color: white; text-decoration: none; font-weight: bold; }");
          client.println(".toggle span { font-weight: bold; }");
          client.println("</style></head><body>");

          client.println("<h2>GrowMate Dashboard</h2>");

          // System status
          client.println("<div class='section'><p><strong>Current State:</strong> ");
          switch (currentState) {
            case IDLE: client.print("IDLE"); break;
            case WATERING: client.print("WATERING"); break;
            case WAITING_FOR_FINAL_READING: client.print("WAITING FOR FINAL READING"); break;
          }
          client.println("</p><p><strong>Last Timestamp:</strong> " + lastTimestamp + "</p>");
          client.println("<p><strong>Predicted Watering Time:</strong> " + String(predictedTime) + " seconds</p></div>");

          // Sensor Data
          client.println("<div class='section'><h3>Sensor Readings</h3>");
          client.println("<table><tr><th>Temperature (&deg;C)</th><th>Humidity (%)</th><th>Moisture</th><th>Light</th></tr>");
          client.println("<tr><td>" + String(temperature) + "</td><td>" + String(humidity) + "</td><td>" + String(moisture) + "</td><td>" + String(light) + "</td></tr></table></div>");

          // Controls
          client.println("<div class='section'><h3>Device Controls</h3>");
          // Fan status
          String fanStatus = digitalRead(fanPin) ? "<span style='color:green;font-weight:bold;'>ON</span>" : "<span style='color:red;font-weight:bold;'>OFF</span>";
          client.println("<div class='toggle'><span>Fan: " + fanStatus + "</span><a href='/toggleFan'>" + String(digitalRead(fanPin) ? "Turn OFF" : "Turn ON") + "</a></div>");

          // Light status
          String lightStatus = digitalRead(lightPin) ? "<span style='color:green;font-weight:bold;'>ON</span>" : "<span style='color:red;font-weight:bold;'>OFF</span>";
          client.println("<div class='toggle'><span>Light: " + lightStatus + "</span><a href='/toggleLight'>" + String(digitalRead(lightPin) ? "Turn OFF" : "Turn ON") + "</a></div>");

          // Auto-Watering status (already handled similarly)
          String autoStatus = autoWateringEnabled ? "<span style='color:green;font-weight:bold;'>ON</span>" : "<span style='color:red;font-weight:bold;'>OFF</span>";
          client.println("<div class='toggle'><span>Auto-Watering: " + autoStatus + "</span><a href='/toggleAutoWatering'>" + String(autoWateringEnabled ? "Turn OFF" : "Turn ON") + "</a></div>");

          client.println("</div>");

          client.println("<p><em>Page auto-refreshes every 5 seconds.</em></p>");
          client.println("</body></html>");
          break;
        }

        if (c == '\n') currentLine = "";
        else if (c != '\r') currentLine += c;
      }
    }
    client.stop();
  }
}


void clearSerial2Buffer() {
  while (Serial2.available()) {
    Serial2.read();  // Discard bytes
  }
}

String readLineFromSerial() {
  String buffer = "";
  unsigned long start = millis();
  clearSerial2Buffer();  //Clear junk before reading
  const unsigned long timeout = 1000; // 1 sec timeout
  while (millis() - start < timeout) {
    while (Serial2.available()) {
      char c = Serial2.read();

      // Use '#' or '\n' as end-of-message marker
      if (c == '#') {
        return buffer;
      }

      buffer += c;
    }
    delay(5);
  }

  Serial.println("Timeout: Did not receive complete message.");
  return "";
}


void logDataToFirebase() {
  String moistureAfterValue = moisture_after > 0 ? String(moisture_after) : "null";
  
  String fullLog = "{\"" + lastTimestamp + "\": {" +
    "\"moisture_before\": " + String(moisture) + ", " +
    "\"temperature\": " + String(temperature) + ", " +
    "\"humidity\": " + String(humidity) + ", " +
    "\"light\": " + String(light) + ", " +
    "\"predicted_time\": " + String(predictedTime) + ", " +
    "\"moisture_after\": " + moistureAfterValue + "}}";

  logToFirebase("/training_logs", fullLog, true);
}

void loop() {
  // Always handle web clients in a non-blocking way
  handleClient();
  
  // State machine for main program logic
  switch (currentState) {
  case IDLE:
    // Check for incoming data
    if (Serial2.available()) {
      String receivedData = readLineFromSerial();
      
      if (receivedData.length() > 0) {
        Serial.println("Received from K66F: " + receivedData);
        
        // Process initial sensor readings
        temperature = extractValue(receivedData, "temperature");
        humidity = extractValue(receivedData, "humidity");
        moisture = extractValue(receivedData, "moisture");
        light = extractValue(receivedData, "light");

        // Debug output
        Serial.println("Parsed values:");
        Serial.println("Temperature: " + String(temperature));
        Serial.println("Humidity: " + String(humidity));
        Serial.println("Moisture: " + String(moisture));
        Serial.println("Light: " + String(light));

        // Simple validation - if any reading is nan, don't proceed
        if (isnan(temperature) || isnan(humidity) || isnan(moisture) || isnan(light)) {
          Serial.println("Warning: Some sensor readings are invalid, staying in IDLE state");
        } else {
          lastTimestamp = getFormattedTimeEST();
          
          // Check if moisture is below or equal to threshold before proceeding or autoWateringEnabled is of or on
          if (autoWateringEnabled && moisture <= 40) {
            predictedTime = callAIPrediction(temperature, humidity, light, moisture);

            // Send predicted time to K66F over Serial2
            String durationCmd = "{\"water_duration\": " + String(predictedTime) + "}\n";
            Serial2.print(durationCmd);
            Serial.println("Sent to K66F: " + durationCmd);
            
            // Move to watering state
            digitalWrite(5, HIGH);
            currentState = WATERING;
            stateStartTime = millis();
            wateringEndTime = stateStartTime + (predictedTime * 1000);
            
            Serial.println("Moisture level " + String(moisture) + " <= 40, starting watering for " + String(predictedTime) + " seconds");
          } else {
                if (!autoWateringEnabled) {
                  Serial.println("Auto-watering is OFF. Skipping watering even though moisture is low.");
                }
                else {
                  Serial.println("Moisture level " + String(moisture) + " > 40, no watering needed");
                }
          }
        }
      }
    }
    break;
      
    case WATERING:
    // Stop watering immediately if auto-watering is turned OFF mid-process
    if (!autoWateringEnabled) {
      Serial.println("Auto-watering turned OFF during watering. Pump won't run after current cycle is complete!");
      currentState = IDLE;
      stateStartTime = millis();
      moisture_after = -1;
      break;
    }

    // Check if watering time has completed normally
    if (millis() >= wateringEndTime) {
      Serial.println("Watering complete, waiting for final moisture reading...");
      currentState = WAITING_FOR_FINAL_READING;
      stateStartTime = millis();
      moisture_after = -1;
    }
    break;
    case WAITING_FOR_FINAL_READING:
      // Wait 1min before trying to get final reading (for demo 1min - This should be 30min)
      if (millis() - stateStartTime >= 60000) {
        // For final moisture reading
        if (Serial2.available()) {
          String moistureUpdate = readLineFromSerial();
          if (moistureUpdate.length() > 0) {
            Serial.println("Final reading: " + moistureUpdate);
            moisture_after = extractValue(moistureUpdate, "moisture");
            
            if (moisture_after <= 0) {
              Serial.println("Warning: Invalid final moisture reading: " + String(moisture_after));
            }
            //return to idle
            currentState = IDLE;
          }
        } else if (millis() - stateStartTime >= 30000) {
          // Timeout after waiting long enough, log what we have
          Serial.println("Timeout waiting for moisture reading, moving on without it");
          currentState = IDLE;
        }
      }
      break;
  }
  
  delay(10); // Small delay to prevent CPU hogging
}
