//Code for the Keil Studio IDE to run on the K66F
// This code is designed to work with the K66F board and the K66F board.

// ========== Includes & Constants ========== //
#include "mbed.h"
#include <cstring>
using namespace std::chrono;

//-----------------------------------------
// Configuration & Constants
//-----------------------------------------
#define BLINKING_RATE 500ms

//-----------------------------------------
// Serial Communication (ESP32)
//-----------------------------------------
static BufferedSerial serial_port(PTC4, PTC3, 9600);  // UART for ESP32 communication
volatile bool canSendData = true;
Mutex serial_port_mutex;
Mutex sendStateMutex;
//-----------------------------------------
// Threading
//-----------------------------------------
Thread send_data_thread;
Thread read_data_thread;

//-----------------------------------------
// I2C Interfaces
//-----------------------------------------
I2C i2c1(PTC11, PTC10);
I2C i2c2(PTB3, PTB2);

//-----------------------------------------
// I2C Addresses
//-----------------------------------------
const int BH1750_ADDR = 0x23 << 1;  // BH1750 (Light Sensor)
const int SHTC3_ADDR = 0x70 << 1;   // SHTC3 (Temperature & Humidity Sensor)

//-----------------------------------------
// Sensors & Actuators
//-----------------------------------------
AnalogIn soilSensor(PTB7);            // Analog pin for soil moisture
Mutex soilMoistureMutex;              // Mutex to protect soil moisture reading
DigitalOut fanLED(PTC8);              // LED representing fan control
DigitalOut lightLED(PTC16);           // LED representing light control
DigitalOut relayControl(PTA1, 1);     // Relay control for water pump (Active Low)

//-----------------------------------------
// Sensor Functions
//-----------------------------------------
// Function to read soil moisture level
float readSoilMoisture() {
    soilMoistureMutex.lock();
    float voltage = soilSensor.read() * 3.3;  // Convert ADC value to voltage
    float moisturePercent = (1 - (voltage / 3.3)) * 100;  // Convert to %
    soilMoistureMutex.unlock();
    return moisturePercent;
}

// Function to initialize light sensor
void initLightSensor() {
    char cmd[1] = {0x01};  // Power ON
    i2c2.write(BH1750_ADDR, cmd, 1);
    ThisThread::sleep_for(180ms);  // Allow time for power-up
    cmd[0] = 0x10;  // Continuous high-resolution mode
    i2c2.write(BH1750_ADDR, cmd, 1);
    ThisThread::sleep_for(180ms);
}

// Function to read light intensity
float readLightIntensity() {
    char cmd[2] = {0};
    i2c2.read(BH1750_ADDR, cmd, 2);  // Read 2 bytes of light data
    float lux = ((cmd[0] << 8) | cmd[1]) / 1.2;  // Convert raw data to lux
    return lux;
}

// Function to initialize SHTC3 sensor
void initSHTC3() {
    char cmd[2] = {0x35, 0x17};  // Wake up SHTC3
    i2c1.write(SHTC3_ADDR, cmd, 2);
    ThisThread::sleep_for(10ms);
}

// Function to read temperature and humidity from SHTC3
void readSHTC3(float &temperature, float &humidity) {
    char cmd[2] = {0x7C, 0xA2};  // Measurement command
    char data[6] = {0};

    i2c1.write(SHTC3_ADDR, cmd, 2);
    ThisThread::sleep_for(20ms);
    i2c1.read(SHTC3_ADDR, data, 6);

    // Convert raw data to temperature (°C)
    int temp_raw = (data[0] << 8) | data[1];
    temperature = -45 + (175.0 * temp_raw) / 65535.0;

    // Convert raw data to humidity (%RH)
    int hum_raw = (data[3] << 8) | data[4];
    humidity = (100.0 * hum_raw) / 65535.0;
}

// Function to send sensor data to ESP32 in JSON format
void send_data() {
    while (true) {
        sendStateMutex.lock();
        bool send = canSendData;
        sendStateMutex.unlock();
        if(send) {

        float temperature, humidity;
        float soilMoisture = readSoilMoisture();
        float lightIntensity = readLightIntensity();
        readSHTC3(temperature, humidity);
        // Format sensor data in JSON
        char jsonData[128];
        int len = sprintf(jsonData,
            "{\"temperature\": %.2f, \"humidity\": %.2f, \"moisture\": %.2f, \"light\": %.2f}#\n",
            temperature, humidity, soilMoisture, lightIntensity);

        //Send data via UART to ESP32
        serial_port_mutex.lock();
        serial_port.write(jsonData, len);
        serial_port.sync();  // Wait until data is flushed
        serial_port_mutex.unlock();
        //printf("Sent JSON: %s\n", jsonData);
        }
        ThisThread::sleep_for(300ms);

    }
}
void processCommandFromESP(const char* buffer, uint64_t currentTime, uint64_t &pumpStopTime, bool &pumpRunning) {
    // Handle watering command
    const char *waterCmd = strstr(buffer, "\"water_duration\"");
    if (waterCmd) {
        float duration = 0;
        if (sscanf(waterCmd, "\"water_duration\": %f", &duration) == 1) {
            //printf("Parsed watering_duration: %.2f seconds\n", duration);

            sendStateMutex.lock();
            canSendData = false;
            sendStateMutex.unlock();

            if (duration > 0) {
                pumpStopTime = currentTime + (uint64_t)(duration * 1e6);
                relayControl = 0;
                pumpRunning = true;
                printf("Pump started\n");
            } else {
                relayControl = 1;
                sendStateMutex.lock();
                canSendData = true;
                sendStateMutex.unlock();
                pumpRunning = false;
                printf("No watering needed (0 duration)\n");
            }
        }
    }

    // Handle fan/light control
    if (strstr(buffer, "\"fan\": \"on\"")) {
        fanLED = 1;
        printf("Fan ON\n");
    } else if (strstr(buffer, "\"fan\": \"off\"")) {
        fanLED = 0;
        printf("Fan OFF\n");
    }

    if (strstr(buffer, "\"light\": \"on\"")) {
        lightLED = 1;
        printf("Light ON\n");
    } else if (strstr(buffer, "\"light\": \"off\"")) {
        lightLED = 0;
        printf("Light OFF\n");
    }

}

void handleIncomingSerialData(char* buffer, int &index, uint64_t currentTime, uint64_t &pumpStopTime, bool &pumpRunning) {
    char c;
    while (serial_port.readable() && serial_port.read(&c, 1)) {
        if (c == '\n') {
            buffer[index] = '\0';  // Null-terminate string
            printf("Received from ESP32: %s\n", buffer);
            processCommandFromESP(buffer, currentTime, pumpStopTime, pumpRunning);
            index = 0;  // Reset buffer
        } else {
            if (index < 127) {
                buffer[index++] = c;
            }
        }
    }
}

void checkPumpStopCondition(uint64_t currentTime, uint64_t &pumpStopTime, bool &pumpRunning) {
    if (pumpRunning && currentTime >= pumpStopTime) {
        relayControl = 1;  // Turn OFF relay
        sendStateMutex.lock();
        canSendData = true;
        sendStateMutex.unlock();
        pumpRunning = false;
        printf("Pump stopped after scheduled duration\n");
    }
}

void debugPumpStatus(uint64_t currentTime, uint64_t pumpStopTime, bool pumpRunning) {
    static uint64_t lastDebugTime = 0;
    if (currentTime - lastDebugTime > 5000000) {
        if (pumpRunning) {
            printf("DEBUG: Pump is ON, will stop in %.2f seconds\n",
                   (float)(pumpStopTime - currentTime) / 1e6);
        }
        lastDebugTime = currentTime;
    }
}

void read_data() {
    char buffer[128];
    int index = 0;
    uint64_t pumpStopTime = 0;
    bool pumpRunning = false;
    relayControl = 1;  // OFF initially (Active Low)

    while (true) {
        uint64_t currentTime = duration_cast<microseconds>(Kernel::Clock::now().time_since_epoch()).count();

        handleIncomingSerialData(buffer, index, currentTime, pumpStopTime, pumpRunning);
        checkPumpStopCondition(currentTime, pumpStopTime, pumpRunning);
        debugPumpStatus(currentTime, pumpStopTime, pumpRunning);

        ThisThread::sleep_for(20ms);
    }
}


int main() {
    initLightSensor();
    initSHTC3();
    send_data_thread.start(send_data);
    read_data_thread.start(read_data);

    while (1) {
        ThisThread::sleep_for(BLINKING_RATE);
    }
}