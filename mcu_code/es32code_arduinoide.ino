#include <WiFi.h>
#include <HTTPClient.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#define FIREBASE_SECRET "YOUR_FIREBASE_SECRET" // Replace with your Firebase secret

// Set this manually based on season
// -14400 = UTC -4 (EDT), -18000 = UTC -5 (EST)
#define TIMEZONE_OFFSET -14400  

const char *ssid = "YOUR_WIFI_SSID"; // Replace with your WiFi SSID
const char *password = "YOUR_WIFI_PASSWORD"; // Replace with your WiFi password
const char *firebase_url = "YOUR_FIREBASE_URL"; // Replace with your Firebase URL
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
  WiFiClient client = server.available();  // non-blocking check for clients

  if (client) {
    unsigned long clientTimeout = millis() + 5000; // 5 second timeout
    String currentLine = "";
    
    // Process client request with timeout
    while (client.connected() && millis() < clientTimeout) {
      if (client.available()) {
        char c = client.read();
        
        if (c == '\n') {
          if (currentLine.length() == 0) {
            // Send HTTP response
            client.println("HTTP/1.1 200 OK");
            client.println("Content-type:text/html");
            client.println();

            client.println("<!DOCTYPE html><html><head><meta http-equiv='refresh' content='5'><title>ESP32 Sensor Dashboard</title></head><body>");
            client.println("<h2>GrowMate Dashboard</h2>");
            client.print("<p><strong>Current state:</strong> ");

            switch(currentState) {
              case IDLE: client.print("IDLE"); break;
              case WATERING: client.print("WATERING"); break;
              case WAITING_FOR_FINAL_READING: client.print("WAITING FOR FINAL READING"); break;
            }
            client.println("</p>");

            client.println("<h3>Latest Sensor Readings</h3>");
            client.println("<table border='1' cellpadding='8'><tr><th>Temperature (°C)</th><th>Humidity (%)</th><th>Moisture</th><th>Light</th></tr>");
            client.print("<tr><td>" + String(temperature) + "</td><td>" + String(humidity) + "</td><td>" + String(moisture) + "</td><td>" + String(light) + "</td></tr></table>");

            client.println("<br><strong>Last Timestamp:</strong> " + lastTimestamp + "<br>");
            client.println("<strong>Predicted Watering Time:</strong> " + String(predictedTime) + " sec<br>");

            // Links for controlling LED, Fan, and Light
            client.println("<br><a href=\"/H\">Turn LED ON</a><br>");
            client.println("<a href=\"/L\">Turn LED OFF</a><br>");
            client.println("<br><a href=\"/FanOn\">Turn Fan ON</a><br>");
            client.println("<a href=\"/FanOff\">Turn Fan OFF</a><br>");
            client.println("<br><a href=\"/LightOn\">Turn Light ON</a><br>");
            client.println("<a href=\"/LightOff\">Turn Light OFF</a><br>");
            client.println("<br><em>Page auto-refreshes every 5 seconds.</em>");
            client.println("</body></html>");

            client.println();
            break;
          } else {
            currentLine = "";
          }
        } else if (c != '\r') {
          currentLine += c;
        }

        // Test: LED control
        if (currentLine.endsWith("GET /H")) {
          digitalWrite(2, HIGH);  // LED ON
        }
        if (currentLine.endsWith("GET /L")) {
          digitalWrite(2, LOW);   // LED OFF
        }

        // Fan control
        if (currentLine.endsWith("GET /FanOn")) {
          digitalWrite(fanPin, HIGH);  // Fan ON
        }
        if (currentLine.endsWith("GET /FanOff")) {
          digitalWrite(fanPin, LOW);   // Fan OFF
        }

        // Light control
        if (currentLine.endsWith("GET /LightOn")) {
          digitalWrite(lightPin, HIGH);  // Light ON
        }
        if (currentLine.endsWith("GET /LightOff")) {
          digitalWrite(lightPin, LOW);   // Light OFF
        }

      }
    }

    client.stop();
  }
}


String readLineFromSerial() {
  String message = "";
  unsigned long startTime = millis();
  
  // Buffer to collect data with timeout
  while (millis() - startTime < 500) {  // 500ms timeout to receive complete message
    if (Serial2.available()) {
      char c = Serial2.read();
      message += c;
      
      // Check if we have a complete JSON message
      if (c == '\n' || c == '}') {
        // Make sure we have a properly formatted JSON with all expected fields
        if (message.indexOf("{") >= 0 && 
            message.indexOf("}") >= 0 &&
            message.indexOf("temperature") >= 0 && 
            message.indexOf("humidity") >= 0 && 
            message.indexOf("moisture") >= 0 && 
            message.indexOf("light") >= 0) {
          
          // Try to clean up the JSON if needed
          int startPos = message.indexOf("{");
          int endPos = message.indexOf("}") + 1;
          
          if (startPos >= 0 && endPos > startPos) {
            // Extract the JSON part only
            String cleanJson = message.substring(startPos, endPos);
            Serial.println("Cleaned JSON: " + cleanJson);
            return cleanJson;
          }
        }
      }
    }
    delay(100);  // Small delay to prevent CPU hogging
  }
  
  Serial.println("Failed to receive complete valid JSON message within timeout");
  return "";
}

void loop() {
  // Always handle web clients in a non-blocking way
  handleClient();

  // digitalWrite(fanPin, HIGH);
  // delay(1000);  // Wait for 1 second
  // digitalWrite(fanPin, LOW);
  // delay(1000);  // Wait for 1 second
  // digitalWrite(lightPin, HIGH);
  // delay(1000);  // Wait for 1 second
  // digitalWrite(lightPin, LOW);

  
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
          
          // Check if moisture is below or equal to threshold before proceeding
          if (moisture <= 40) {
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
            Serial.println("Moisture level " + String(moisture) + " > 40, no watering needed");
          }
        }
      }
    }
    break;
      
    case WATERING:
      // Check if watering time has completed
      if (millis() >= wateringEndTime) {
        digitalWrite(5, LOW);
        Serial.println("Watering complete, waiting for final moisture reading...");
        currentState = WAITING_FOR_FINAL_READING;
        stateStartTime = millis();
        moisture_after = -1; // Reset final moisture reading
      }
      break;
      
    case WAITING_FOR_FINAL_READING:
      // Wait 1min before trying to get final reading
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
            
            // Log data regardless of validity and return to idle
            logDataToFirebase();
            currentState = IDLE;
          }
        } else if (millis() - stateStartTime >= 30000) {
          // Timeout after waiting long enough, log what we have
          Serial.println("Timeout waiting for moisture reading, moving on without it");
          logDataToFirebase(); // Log with missing moisture_after
          currentState = IDLE;
        }
      }
      break;
  }
  
  delay(10); // Small delay to prevent CPU hogging
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