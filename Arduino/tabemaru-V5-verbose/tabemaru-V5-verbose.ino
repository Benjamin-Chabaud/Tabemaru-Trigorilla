#include <Wire.h>
#include "DHT20.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <SD.h>

// --- SENSOR OBJECTS ---
// Two DHT20 temperature and humidity sensors
DHT20 dhtTop;
DHT20 dhtBot;

// --- I2C MULTIPLEXER (TCA9548A) CONTROL ---
// The multiplexer allows multiple I2C devices with the same address to coexist on the bus.
#define TCAADDR 0x70

// Function to switch the active channel on the I2C multiplexer
void tcaSelect(uint8_t i) {
  if (i > 7) return; // The TCA9548A only has 8 channels (0 to 7)
  Wire.beginTransmission(TCAADDR);
  Wire.write(1 << i);
  Wire.endTransmission();
  delay(10); // Short delay to ensure the switch is completed before communication
}

// --- OLED DISPLAY DEFINITIONS ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET    -1 // -1 means the display shares the Arduino reset pin
#define SCREEN_ADDRESS 0x3C // Default I2C address for 128x32 SSD1306 OLEDs
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- SD CARD DEFINITIONS ---
const int chipSelect = 53; // SPI Chip Select (CS) pin for the SD card reader on Trigorilla (Mega 2560)
bool sdReady = false;      // Flag indicating if the SD card is successfully initialized and ready
bool safeToEject = false;  // Flag indicating if the user requested to safely eject the SD card

// --- PIN DEFINITIONS (TRIGORILLA BOARD) ---
const int buttonXMinPin = 3;  // Sequence start button (connected to X- endstop pin)
const int buttonZMinPin = 18; // LED toggle and SD eject button (connected to Z- endstop pin)
const int motorPin  = 9;      // Motor control pin
const int valve1Pin = 7;      // First valve control pin
const int valve2Pin = 44;     // Second valve control pin
const int ledPin    = 10;     // Internal illumination LED control pin
const int hotbedPin = 8;      // Hotbed heating element control pin
const int bedTempPin = A15;   // Hotbed thermistor analog input pin (T2 port)

// --- TIMERS FOR NON-BLOCKING DELAYS ---
// We use millis() instead of delay() so the program never freezes
unsigned long previousLcdMillis = 0;
const long lcdInterval = 250;  // Refresh OLED every 250ms
unsigned long previousTempMillis = 0;
const long tempInterval = 500; // Read hotbed temperature every 500ms
unsigned long previousDHTMillis = 0;
const long dhtInterval = 2000; // Read DHT20 sensors every 2000ms (2 seconds)
unsigned long previousSDMillis = 0;
const long sdInterval = 10000; // Attempt SD card reconnection every 10 seconds

// --- SEQUENCE TIMINGS (in milliseconds) ---
const unsigned long timeValve1 = 100000; // Step 1: Valve 1 remains open for 1 minute and 40 seconds
const unsigned long timeMotor  = 5000;   // Step 2: Motor runs for 5 seconds
const unsigned long timeValve2 = 140000; // Step 3: Valve 2 remains open for 2 minutes and 20 seconds
const unsigned long timeHotbed = 800000; // Total hotbed activation time, which also dictates the end of the entire sequence

// --- STATE MEMORY ---
bool ledState = LOW;                  // Current state of the illumination LED
int sequenceState = 0;                // Current step of the operational sequence (0 = Idle, 1-4 = Active)
unsigned long sequenceStartTime = 0;  // Timestamp when the sequence was started

// --- HARDWARE INTERRUPT VARIABLES ---
// Using volatile variables because they are modified inside Interrupt Service Routines (ISRs)
volatile unsigned long xFallTime = 0;
volatile bool flagXPressed = false;   // Flag raised when the X button is pressed

volatile unsigned long zFallTime = 0;
volatile bool zCurrentlyPressed = false; // Real-time state of the Z button
volatile bool zStateChanged = false;     // Flag raised when the Z button state changes
bool longPressHandled = false;           // Prevents multiple triggers for a single long press
const unsigned long LONG_PRESS_TIME = 1500; // Holding Z button for 1.5 seconds triggers SD Eject

// Interrupt Service Routine for Button X (Sequence Start)
void isr_X() {
  if (millis() - xFallTime > 200) { // Software debounce: ignore signals closer than 200ms
    flagXPressed = true;
    xFallTime = millis();
  }
}

// Interrupt Service Routine for Button Z (LED Toggle / SD Eject)
void isr_Z() {
  bool st = digitalRead(buttonZMinPin);
  zCurrentlyPressed = (st == LOW); // LOW means the button is pressed (INPUT_PULLUP logic)
  if (zCurrentlyPressed) {
    zFallTime = millis(); // Record the exact time the button was pressed down
  }
  zStateChanged = true;
}

// --- THERMOSTAT VARIABLES ---
const float targetTemp = 105.0; // Target temperature for the hotbed in Celsius
const float hysteresis = 1.0;   // Allowable temperature drift before re-engaging the heater
bool regulateHotbed = false;    // Flag enabling the thermostat regulation logic
float currentBedTemp = 0.0;     // Last read temperature of the hotbed
bool isHeatingActive = false;   // Flag tracking if the heating element is currently ON

// Steinhart-Hart equation constants for 100k NTC Thermistor
const float R1 = 4700.0;   // Pull-up resistor on the Trigorilla board (4.7k Ohm)
const float R0 = 100000.0; // Nominal resistance of the thermistor at 25C (100k Ohm)
const float T0 = 298.15;   // Nominal temperature in Kelvin (25C)
const float B = 3950.0;    // Beta coefficient of the thermistor

// --- EVENT LOGGING FUNCTION ---
// Formats and outputs system state to both the Serial Monitor and the SD Card CSV file.
void logEvent(String eventName) {
  unsigned long currentMillis = millis();
  
  // Resolve boolean states for logging based on the current sequence step
  bool v1State = (sequenceState == 1);
  bool mState = (sequenceState == 2);
  bool v2State = (sequenceState == 3);
  bool hotbedState = (regulateHotbed && isHeatingActive);

  // 1. Output to Serial Monitor (Human-readable format)
  Serial.print("[LOG] Time: "); Serial.print(currentMillis);
  Serial.print(" | Event: "); Serial.print(eventName);
  Serial.print(" | Top: "); Serial.print(dhtTop.getTemperature(), 1); Serial.print("C/"); Serial.print(dhtTop.getHumidity(), 1); Serial.print("%");
  Serial.print(" | Bot: "); Serial.print(dhtBot.getTemperature(), 1); Serial.print("C/"); Serial.print(dhtBot.getHumidity(), 1); Serial.print("%");
  Serial.print(" | Bed: "); Serial.print(currentBedTemp, 1); Serial.print("C");
  Serial.print(" | SEQ: "); Serial.print(sequenceState);
  Serial.print(" | LED:"); Serial.print(ledState ? 1 : 0);
  Serial.print(" V1:"); Serial.print(v1State ? 1 : 0);
  Serial.print(" V2:"); Serial.print(v2State ? 1 : 0);
  Serial.print(" M:"); Serial.print(mState ? 1 : 0);
  Serial.print(" HB:"); Serial.println(hotbedState ? 1 : 0); 

  // 2. Output to SD Card (CSV format for data analysis)
  if (sdReady && !safeToEject) {
    File dataFile = SD.open("LOG.CSV", FILE_WRITE);
    if (dataFile) {
      dataFile.print(currentMillis); dataFile.print(",");
      dataFile.print(eventName); dataFile.print(",");
      dataFile.print(dhtTop.getTemperature(), 1); dataFile.print(",");
      dataFile.print(dhtTop.getHumidity(), 1); dataFile.print(",");
      dataFile.print(dhtBot.getTemperature(), 1); dataFile.print(",");
      dataFile.print(dhtBot.getHumidity(), 1); dataFile.print(",");
      dataFile.print(currentBedTemp, 1); dataFile.print(",");
      dataFile.print(sequenceState); dataFile.print(",");
      dataFile.print(ledState ? 1 : 0); dataFile.print(",");
      dataFile.print(v1State ? 1 : 0); dataFile.print(",");
      dataFile.print(v2State ? 1 : 0); dataFile.print(",");
      dataFile.print(mState ? 1 : 0); dataFile.print(",");
      dataFile.println(hotbedState ? 1 : 0); 
      
      dataFile.close();
    } else {
      // If the file fails to open, assume the SD card was abruptly removed
      sdReady = false;
      Serial.println("[CRITICAL ERROR] SD Card lost during event writing!");
    }
  }
}

// --- SETUP ROUTINE ---
// Runs once at power-on to initialize all hardware components.
void setup() {
  Serial.begin(115200);
  Serial.println("==================================");
  Serial.println("[SYSTEM] Booting up Tabemaru V5...");
  
  Serial.println("[SYSTEM] Initializing Actuator Pins...");
  // Configure output pins for relays, motor, and heating element
  pinMode(motorPin, OUTPUT);
  pinMode(valve1Pin, OUTPUT);
  pinMode(valve2Pin, OUTPUT);
  pinMode(ledPin, OUTPUT);
  pinMode(hotbedPin, OUTPUT);

  // Set initial safe states (everything OFF) without triggering log events yet
  regulateHotbed = false; 
  digitalWrite(hotbedPin, LOW); 
  digitalWrite(valve1Pin, LOW);
  digitalWrite(motorPin, LOW);
  digitalWrite(valve2Pin, LOW);
  sequenceState = 0; 
  isHeatingActive = false;
  
  digitalWrite(ledPin, LOW);

  // Configure input pins for buttons with internal pull-up resistors
  pinMode(buttonXMinPin, INPUT_PULLUP);
  pinMode(buttonZMinPin, INPUT_PULLUP);

  Serial.println("[SYSTEM] Attaching Hardware Interrupts...");
  // Attach interrupts for zero-latency button responses
  attachInterrupt(digitalPinToInterrupt(buttonXMinPin), isr_X, FALLING); // Triggers when pressed (HIGH to LOW)
  attachInterrupt(digitalPinToInterrupt(buttonZMinPin), isr_Z, CHANGE);  // Triggers on press and release

  Serial.println("[SYSTEM] Starting I2C Bus...");
  Wire.begin();
  
  // Initialize DHT20 Sensors through the multiplexer
  Serial.println("[SYSTEM] Initializing DHT20 Sensors...");
  tcaSelect(0); // Select multiplexer channel 0 for the top sensor
  if(dhtTop.begin()) { Serial.println("[SYSTEM] TOP DHT20 connected on SC0."); }
  else { Serial.println("[ERROR] TOP DHT20 failed to connect!"); }
  
  tcaSelect(3); // Select multiplexer channel 3 for the bottom sensor
  if(dhtBot.begin()) { Serial.println("[SYSTEM] BOTTOM DHT20 connected on SC3."); }
  else { Serial.println("[ERROR] BOTTOM DHT20 failed to connect!"); }
  
  // Initialize the OLED Display
  Serial.println("[SYSTEM] Initializing OLED Display...");
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("[ERROR] OLED Screen failed!");
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0,0);
    display.println("System Ready...");
    display.display();
    delay(500); // Briefly show the ready message
  }

  // Initialize the SD Card communication via SPI
  Serial.print("[SYSTEM] Initializing SD card...");
  pinMode(53, OUTPUT); // Hardware SS pin must be OUTPUT on Mega for SPI to work

  if (!SD.begin(chipSelect)) {
    Serial.println(" FAILED! (Will retry in background)");
    sdReady = false;
  } else {
    Serial.println(" OK!");
    sdReady = true;
    
    // Create or open the CSV file and write the header row if the file is new
    File dataFile = SD.open("LOG.CSV", FILE_WRITE);
    if (dataFile) {
      if (dataFile.size() == 0) {
        dataFile.println("Time_ms,Event,Top_Temp_C,Top_Hum_%,Bot_Temp_C,Bot_Hum_%,Bed_Temp_C,Sequence,LED_State,Valve1,Valve2,Motor,Hotbed");
      }
      dataFile.close();
      logEvent("SYSTEM_BOOT"); // Log the initial boot event
    }
  }
}

// --- STOP SEQUENCE FUNCTION ---
// Safely terminates any active sequence, turns off all actuators, and resets the state machine.
void stopSequence() {
  regulateHotbed = false; 
  digitalWrite(hotbedPin, LOW); 
  digitalWrite(valve1Pin, LOW);
  digitalWrite(motorPin, LOW);
  digitalWrite(valve2Pin, LOW);
  sequenceState = 0; 
  isHeatingActive = false;
  logEvent("SEQ_STOPPED");
}

// --- MAIN CONTROL LOOP ---
// Executes continuously, handling thermostats, buttons, sequence progression, sensors, and displays.
void loop() {
  unsigned long currentMillis = millis(); 

  // --- 0. THERMOSTAT MANAGEMENT ---
  // Evaluates the hotbed thermistor and regulates heating
  if (currentMillis - previousTempMillis >= tempInterval) {
    previousTempMillis = currentMillis;
    int rawValue = analogRead(bedTempPin);
    
    // Safety check: Detect disconnected (near 1023) or shorted (near 0) thermistor
    if (rawValue < 10 || rawValue > 1010) {
      if (regulateHotbed || currentBedTemp != -1.0) {
        digitalWrite(hotbedPin, LOW); // Force heater off for safety
        currentBedTemp = -1.0;        // Indicate error state
        if (regulateHotbed) {
          logEvent("HOTBED_ERROR");
          stopSequence();             // Abort sequence if heating fails
        }
      }
    } else {
      // Calculate temperature using the Steinhart-Hart equation
      float R2 = R1 * ((float)rawValue / (1023.0 - (float)rawValue));
      float tempKelvin = 1.0 / ( (1.0 / T0) + (1.0 / B) * log(R2 / R0) );
      currentBedTemp = tempKelvin - 273.15; // Convert Kelvin to Celsius

      // Thermostat Regulation Logic
      if (regulateHotbed) {
        // Turn ON if temperature drops below the target minus hysteresis
        if (currentBedTemp < (targetTemp - hysteresis) && !isHeatingActive) {
          digitalWrite(hotbedPin, HIGH); 
          isHeatingActive = true;
          logEvent("HOTBED_HEATING_ON");
        } 
        // Turn OFF if temperature reaches or exceeds the target
        else if (currentBedTemp >= targetTemp && isHeatingActive) {
          digitalWrite(hotbedPin, LOW); 
          isHeatingActive = false;
          logEvent("HOTBED_HEATING_OFF");
        }
      } else {
        // Ensure heater remains off if regulation is disabled
        if (isHeatingActive) {
           digitalWrite(hotbedPin, LOW);
           isHeatingActive = false;
           logEvent("HOTBED_HEATING_OFF_FORCED");
        }
      }
    }
  }

  // --- 1. X- BUTTON CHECK (START/STOP SEQUENCE) ---
  // Evaluates the flag raised by the hardware interrupt
  if (flagXPressed) {
    flagXPressed = false; // Acknowledge the press
    logEvent("BUTTON_X_PRESSED");
    
    // If system is idle, start the sequence
    if (sequenceState == 0) {
      sequenceState = 1;
      sequenceStartTime = currentMillis; 
      regulateHotbed = true;         // Start heating the bed
      digitalWrite(valve1Pin, HIGH); // Open the first valve
      logEvent("SEQ_START_STEP1_V1_ON");
    } else {
      // If sequence is already running, pressing X acts as an Emergency Stop
      stopSequence();
    }
  }

  // --- 2. Z- BUTTON CHECK (LED TOGGLE & SD EJECT) ---
  // Evaluates real-time state updated by the hardware interrupt
  if (zCurrentlyPressed) {
    // Check if the button has been held down long enough to trigger SD Eject
    if ((currentMillis - zFallTime >= LONG_PRESS_TIME) && !longPressHandled) {
      safeToEject = !safeToEject; // Toggle ejection state
      
      if (safeToEject) {
        sdReady = false; // Disconnect software link to SD
        Serial.println("\n[SYSTEM] *** SD CARD SAFELY EJECTED ***");
        logEvent("SD_EJECT_COMMAND"); // Logs to serial only, as sdReady is now false
      } else {
        Serial.println("\n[SYSTEM] *** SD CARD MOUNT COMMAND ***");
        logEvent("SD_MOUNT_COMMAND");
      }
      longPressHandled = true; // Prevent re-triggering until button is released
    }
  } else {
    // Button has been released
    if (zStateChanged) {
      zStateChanged = false; // Acknowledge the release
      
      // If it was a short press (less than LONG_PRESS_TIME but more than 50ms for debounce), toggle LED
      if (!longPressHandled && (currentMillis - zFallTime > 50)) {
        ledState = !ledState; 
        digitalWrite(ledPin, ledState ? HIGH : LOW);
        logEvent(ledState ? "LED_ON" : "LED_OFF");
      }
      longPressHandled = false; // Reset the long press tracker
    }
  }

  // --- 3. SEQUENCE PROGRESSION LOGIC ---
  // Automatically advances the sequence based on elapsed time
  if (sequenceState > 0) {
    unsigned long elapsed = currentMillis - sequenceStartTime;
    
    // Step 1: Valve 1 remains open for timeValve1
    if (sequenceState == 1 && elapsed >= timeValve1) {
      digitalWrite(valve1Pin, LOW);  // Close Valve 1
      digitalWrite(motorPin, HIGH);  // Start Motor
      sequenceState = 2;
      logEvent("SEQ_STEP2_V1_OFF_M_ON");
    }
    // Step 2: Motor runs for timeMotor
    else if (sequenceState == 2 && elapsed >= (timeValve1 + timeMotor)) { 
      digitalWrite(motorPin, LOW);   // Stop Motor
      digitalWrite(valve2Pin, HIGH); // Open Valve 2
      sequenceState = 3;
      logEvent("SEQ_STEP3_M_OFF_V2_ON");
    }
    // Step 3: Valve 2 remains open for timeValve2
    else if (sequenceState == 3 && elapsed >= (timeValve1 + timeMotor + timeValve2)) { 
      digitalWrite(valve2Pin, LOW);  // Close Valve 2
      sequenceState = 4;
      logEvent("SEQ_STEP4_V2_OFF_WAIT"); // Wait state while hotbed continues heating
    }
    // Step 4: System waits until timeHotbed is reached, then terminates
    else if (sequenceState == 4 && elapsed >= timeHotbed) { 
      logEvent("SEQ_FINISHED");
      stopSequence();
    }
  }

  // --- 4. DUAL DHT20 SENSOR POLLING ---
  if (currentMillis - previousDHTMillis >= dhtInterval) {
    previousDHTMillis = currentMillis; 
    
    tcaSelect(0); dhtTop.read(); // Read top sensor via multiplexer channel 0
    tcaSelect(3); dhtBot.read(); // Read bottom sensor via multiplexer channel 3

    logEvent("PERIODIC_LOG"); // Log all environmental data to Serial and SD
  }
  
  // --- 5. OLED DISPLAY UPDATE ---
  if (currentMillis - previousLcdMillis >= lcdInterval) {
    previousLcdMillis = currentMillis;
    
    // Determine active actuator states for display
    bool v1State = (sequenceState == 1);
    bool mState = (sequenceState == 2);
    bool v2State = (sequenceState == 3);

    // Determine string representation of the hotbed status
    String bedStatus = "";
    if (currentBedTemp == -1.0) bedStatus = "ERR";
    else if (regulateHotbed && isHeatingActive) bedStatus = "HEAT";
    else if (regulateHotbed && !isHeatingActive) bedStatus = "HOLD";
    else bedStatus = "OFF";

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    
    // Row 1: System Status
    display.setCursor(0,0);
    display.print("SEQ:"); display.print(sequenceState); 
    display.print(" L:"); display.print(ledState ? "ON " : "OFF");
    display.print(" SD:"); 
    if (safeToEject) display.println("EJ");
    else if (sdReady) display.println("OK");
    else display.println("NO");

    // Row 2: Actuator Status
    display.setCursor(0,8);
    display.print("V1:"); display.print(v1State ? "ON " : "OFF ");
    display.print("V2:"); display.print(v2State ? "ON " : "OFF ");
    display.print("M:");  display.println(mState ? "ON" : "OFF");

    // Row 3: Environmental Data (Alternates between Top and Bottom sensors every 5 seconds)
    display.setCursor(0,16);
    bool showTopData = (currentMillis / 5000) % 2 == 0;
    if (showTopData) {
      display.print("AirT:"); display.print(dhtTop.getTemperature(), 1);
      display.print("C H:"); display.print(dhtTop.getHumidity(), 0); display.println("%");
    } else {
      display.print("AirB:"); display.print(dhtBot.getTemperature(), 1);
      display.print("C H:"); display.print(dhtBot.getHumidity(), 0); display.println("%");
    }

    // Row 4: Hotbed Status
    display.setCursor(0,24);
    display.print("Bed: ");
    if(currentBedTemp == -1.0) display.print("ERR");
    else display.print(currentBedTemp, 0);
    display.print("C ["); display.print(bedStatus); display.print("]");

    display.display();
  }

  // --- 6. SD CARD RECONNECTION (HOT-SWAP BACKGROUND TASK) ---
  if (currentMillis - previousSDMillis >= sdInterval) {
    previousSDMillis = currentMillis;

    // Only attempt reconnection if the user hasn't explicitly ejected the card
    if (!safeToEject) {
      if (!sdReady) {
        Serial.println("[SYSTEM] SD Card missing. Attempting to reconnect...");
        digitalWrite(chipSelect, HIGH); // Pulse CS to reset SPI state
        delay(1);
        if (SD.begin(chipSelect)) {
          sdReady = true;
          Serial.println("[SYSTEM] SD Card RECONNECTED successfully.");
          logEvent("SD_RECONNECTED");
        } else {
          Serial.println("[ERROR] SD Card reconnection FAILED.");
        }
      }
    }
  }
}
