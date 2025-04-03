#include "mbed.h"
#include <cstring>

// Blinking rate in milliseconds
#define BLINKING_RATE 500ms

static BufferedSerial serial_port(PTC4, PTC3, 9600);  // UART for ESP32 communication

Mutex serial_port_mutex;
Mutex soilMoistureMutex;

Thread send_data_thread;
Thread read_data_thread;
//Thread pump_control_thread;


// I2C interface for sensors
I2C i2c1(PTC11, PTC10);
I2C i2c2(PTB3, PTB2);

const int BH1750_ADDR = 0x23 << 1;  // BH1750 (Light Sensor)
const int SHTC3_ADDR = 0x70 << 1;   // SHTC3 (Temperature & Humidity Sensor)

// Analog input for Soil Moisture Sensor
AnalogIn soilSensor(PTB7);
DigitalOut led(LED1);

//Relay
// Digital output for relay control (PTA1 or any available pin)
DigitalOut relayControl(PTA1, 1);  // Assuming you are using PTA1 for relay control
// Threshold for soil moisture to trigger pump
#define SOIL_MOISTURE_THRESHOLD 40.0  // Below 30% moisture, turn on the pump

// Function to read soil moisture level
float readSoilMoisture() {
    soilMoistureMutex.lock();
    float voltage = soilSensor.read() * 3.3;  // Convert ADC value to voltage
    float moisturePercent = (1 - (voltage / 3.3)) * 100;  // Convert to %

    //printf("Soil Moisture: %.2f%%\n", moisturePercent);
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
    //printf("Light Intensity: %.2f lux\n", lux);
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

    //printf("Temperature: %.2f°C, Humidity: %.2f%%\n", temperature, humidity);
}

// Function to control the relay (turn on/off pump based on soil moisture)
// void controlPump() {
//     while (true) {
//         float soilCurrentMoisture = readSoilMoisture();

//         if (soilCurrentMoisture < SOIL_MOISTURE_THRESHOLD) {
//             // If soil moisture is below threshold, turn on the pump (activate relay)
//             relayControl = 0;
//             printf("Pump ON\n");
//         } else {
//             // Otherwise, turn off the pump (deactivate relay)
//             relayControl = 1;
//             printf("Pump OFF\n");
//         }

//         ThisThread::sleep_for(1s);  // Check every second
//     }
// }

// Function to send **real sensor data** to ESP32 in JSON format
void send_data() {
    while (true) {
        float temperature, humidity;
        float soilMoisture = readSoilMoisture();
        float lightIntensity = readLightIntensity();
        readSHTC3(temperature, humidity);
        // Format sensor data in JSON
        char jsonData[128];
        sprintf(jsonData,
                "{\"temperature\": %.2f, \"humidity\": %.2f, \"moisture\": %.2f, \"light\": %.2f}\n",
                temperature, humidity, soilMoisture, lightIntensity);

        //Send data via UART to ESP32
        serial_port_mutex.lock();
        serial_port.write(jsonData, strlen(jsonData));
        serial_port_mutex.unlock();

        //printf("Sent JSON: %s\n", jsonData);
        ThisThread::sleep_for(1s);
    }
}

// Function to read data from ESP32 (for future use)
void read_data() {
    char buffer[128];
    int index = 0;
    
    // Variables for pump control
    uint64_t pumpStopTime = 0;  // Time when pump should stop (in microseconds)
    bool pumpRunning = false;
    relayControl = 1;  // Relay OFF initially (Active Low = 0)

    while (true) {
        // Check current time for pump control
        uint64_t currentTime = Kernel::get_ms_count() * 1000;  // Current time in microseconds
        
        // Check for incoming data
        char c;
        while (serial_port.readable() && serial_port.read(&c, 1)) {
            if (c == '\n') {
                buffer[index] = '\0';  // Terminate string
                printf("Received from ESP32: %s\n", buffer);

                // Parse for "water_duration"
                char *start = strstr(buffer, "\"water_duration\"");
                if (start) {
                    float duration = 0;
                    if (sscanf(start, "\"water_duration\": %f", &duration) == 1) {
                        printf("Parsed watering_duration: %.2f seconds\n", duration);

                        if (duration > 0) {
                            // Calculate absolute stop time
                            pumpStopTime = currentTime + (uint64_t)(duration * 1e6);
                            
                            // Turn on pump
                            relayControl = 0;  // Turn ON relay (Active Low)
                            pumpRunning = true;
                            printf("Pump started\n");
                        } else {
                            // No watering needed
                            relayControl = 1;  // Turn OFF relay
                            pumpRunning = false;
                            printf("No watering needed (0 duration)\n");
                        }
                    }
                }

                index = 0;  // Reset buffer
            } else {
                if (index < sizeof(buffer) - 1) {
                    buffer[index++] = c;
                }
            }
        }

        // Check if it's time to stop the pump
        if (pumpRunning && currentTime >= pumpStopTime) {
            relayControl = 1;  // Turn OFF relay
            pumpRunning = false;
            printf("Pump stopped after scheduled duration\n");
        }

        // Add a debugging check to output pump status periodically
        static uint64_t lastDebugTime = 0;
        if (currentTime - lastDebugTime > 5000000) {  // Debug every 5 seconds
            if (pumpRunning) {
                printf("DEBUG: Pump is ON, will stop in %.2f seconds\n", 
                       (float)(pumpStopTime - currentTime) / 1e6);
            }
            lastDebugTime = currentTime;
        }

        ThisThread::sleep_for(20ms);
    }
}


int main() {
    initLightSensor();  // Initialize light sensor
    initSHTC3();        // Initialize SHTC3 sensor
    send_data_thread.start(send_data);
    read_data_thread.start(read_data);
    //pump_control_thread.start(controlPump);  // Start pump control thread

    while (1) {
        ThisThread::sleep_for(BLINKING_RATE);
    }
}
