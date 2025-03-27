#include <Wire.h>
#include <HTTPUpdate.h>
#include <LiquidCrystal_PCF8574.h>
#include <EEPROM.h>
#include <esp_system.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <ArduinoJson.h> 
#include <WiFiClientSecure.h>
#include <WebSocketsClient.h> 


LiquidCrystal_PCF8574 lcd(0x27); 


const char* backendUrl = "https://carwash-api.chrometechnology.shop/api/transactions/";
const char* serviceNames[4] = {"Water", "Soap", "Air", "Gripo"};
#define ACTIVATION_FLAG_ADDRESS 90
#define VENDOR_ID_ADDRESS  61
bool machineActivated = false;
int machineVendorId = 0; 
String machineLicenseKey = "";

// Create a global WebSocketsClient for status and activation instance.
WebSocketsClient activationWebSocket;
WebSocketsClient statusWebSocket;

//Network flow enhancement
unsigned long lastNetworkCheck = 0;
const unsigned long NETWORK_INTERVAL = 50;
unsigned long lastCountdownUpdate = 0;

// WebSocket backend settings:
const char* ws_server = "carwash-api.chrometechnology.shop";
const uint16_t ws_port = 443;
const char* ws_path = "/ws/activate/";
bool updateDisplay = false;
unsigned long displayTimestamp = 0;
const unsigned long displayDelay = 5000;

//Machine Status
unsigned long lastStatusSend = 0;
const unsigned long statusInterval = 10000;
String ws_status_path_str = "/ws/machine-status/";
unsigned long activationMessageExpiry = 0;
String activationMessageType = "";

// Reset Sales
enum ResetSalesState {
  RS_INIT,
  RS_WAIT_CONFIRM,
  RS_CONFIRMED,
  RS_CANCELLED,
  RS_DONE
};

ResetSalesState resetSalesState = RS_DONE;
unsigned long resetSalesStartTime = 0;
unsigned long resetSalesDebounceTime = 0;

// --- Transaction Queue Implementation ---
#define TRANSACTION_QUEUE_SIZE 10
int pendingTransactionAmount[4] = {0, 0, 0, 0};
bool transactionSent[4] = {false, false, false, false};
struct Transaction {
  int index;   // Which feature (0 to 3)
  int amount;  // The amount of credits used for the transaction
};

Transaction transactionQueue[TRANSACTION_QUEUE_SIZE];
int transactionQueueHead = 0;
int transactionQueueTail = 0;

// Check if the transaction queue is empty
bool isTransactionQueueEmpty() {
  return transactionQueueHead == transactionQueueTail;
}

// Enqueue a transaction; returns false if the queue is full
bool enqueueTransaction(int index, int amount) {
  int nextTail = (transactionQueueTail + 1) % TRANSACTION_QUEUE_SIZE;
  if (nextTail == transactionQueueHead) {
    // Queue is full; you may choose to handle overflow as needed.
    return false;
  }
  transactionQueue[transactionQueueTail].index = index;
  transactionQueue[transactionQueueTail].amount = amount;
  transactionQueueTail = nextTail;
  return true;
}

// Dequeue a transaction; returns false if empty
bool dequeueTransaction(Transaction &tx) {
  if (isTransactionQueueEmpty()) {
    return false;
  }
  tx = transactionQueue[transactionQueueHead];
  transactionQueueHead = (transactionQueueHead + 1) % TRANSACTION_QUEUE_SIZE;
  return true;
}

// Process queued transactions (call this in loop on a fixed interval)
unsigned long lastTransactionQueueProcess = 0;
const unsigned long TRANSACTION_PROCESS_INTERVAL = 100; // e.g., every 100 ms

//Edit Main Screen Display
char line1[21] = "Chrome Tech";   // Adjust the first column name using Change Display in the admin settings.
char line2[21] = "Car Wash Vendo"; // Edit second line of the main screen of your choice to display in the main screen (default)
char line3[21] = "Water|Soap|Air|Gripo"; // Edit third line of the main screen of your choice to display in the main screen.(default)

// Must not overlap with the existing Address Values
// Timer EEPROM addresses
const int waterTimerAddress = 32;
const int soapTimerAddress = 36;
const int airTimerAddress = 40;
#define DISPLAY_NAME_ADDRESS 59
const int gripoTimerAddress = 44;

// Sales EEPROM addresses
const int CH1SalesAddress = 16;
const int CH2SalesAddress = 20;
const int CH3SalesAddress = 24;
const int CH4SalesAddress = 28;

const int pauseResumeStateAddress = 50; 
const int timeIncrementAddress = 48; 
#define PAUSE_RESUME_SETTING_ADDR 54

// Admin settings Screen Display turn-over 
bool inAdminMode = false;       

unsigned long lastInteractionTime = 0; // Tracks the last interaction time
const unsigned long INACTIVITY_TIMEOUT = 10000; // 15 seconds timeout

bool isButtonPressed = false; // Tracks button state
bool resetScheduled = false;
unsigned long resetScheduledTime = 0;

// Pin definitions
const uint8_t coinSlot = 19;
const uint8_t waterButton = 5;
const uint8_t soapButton = 18;
const uint8_t airButton = 16;
const uint8_t gripoButton = 17;
const uint8_t waterOutput = 32; 
const uint8_t soapOutput = 33;
const uint8_t airOutput = 25;
const uint8_t gripoOutput = 26;
const uint8_t waterIndicatorPin = 13;
const uint8_t soapIndicatorPin = 12;
const uint8_t airIndicatorPin = 14;
const uint8_t gripoIndicatorPin = 27;




const int debounceTime = 100;  // Increased debounce time (100ms for better reliability)
unsigned long previousMillis[4] = {0, 0, 0, 0};
char lcdBuffer[20];
// Variables for coin system
int pulse = 0;
unsigned long lastCoinMillis = 0;  // For debouncing coin insertion
int previousCoinSlotStatus = HIGH;  // Assuming HIGH is the default state when no coin is inserted
int coinSlotPrevState = HIGH;  

const unsigned long interval = 1000;  // 1 second
bool showInsertCoins = true; // State for blinking text
unsigned long coinspreviousMillis = 0; // Timer for blinking the text "---Insert Coins ---"
bool activeFeature[4] = {false, false, false, false};  // Track if a feature is active

// Flag to control which screen to display
bool displayingAmtPaid = false;
bool coinInserted = false;  

//Output declaration
bool isPaused[4] = {false, false, false, false};
const int buttonPins[] = {waterButton, soapButton, airButton, gripoButton};
bool isAnyButtonPressed() {
    static unsigned long lastButtonPressTime = 0;
    unsigned long currentMillis = millis();
    
    for (int i = 0; i < sizeof(buttonPins) / sizeof(buttonPins[0]); i++) {
        if (digitalRead(buttonPins[i]) == LOW) {
            if (currentMillis - lastButtonPressTime > 300) {  // 300ms debounce
                lastButtonPressTime = currentMillis;
                return true;
            }
        }
    }
    return false;
}
const int outputPins[] = {waterOutput, soapOutput, airOutput, gripoOutput};
const int indicatorPins[] = {waterIndicatorPin, soapIndicatorPin, airIndicatorPin, gripoIndicatorPin};
bool outputs[4] = {false, false, false, false}; 


unsigned long lastButtonPress = 0;
float remainingTime[4] = {0.0, 0.0, 0.0, 0.0};
int totalCredits = 0;
int initialCredits = 1;
bool isMainScreenDisplayed = true;
float waterSwitchIncrement = 120.0;
float soapSwitchIncrement = 30.0;
float airSwitchIncrement = 120.0;
float gripoSwitchIncrement = 60.0;
int lastFeatureIndex = -1; // Track the last feature index that was pressed
static unsigned long timerFinishedStartTime = 0; // Track when the timers finished
unsigned long lastCoinInsertTime = 0;  // Tracks last coin insert time
bool isCountdownActive = false;  // Tracks if the countdown is running
int countdownSeconds = 1;  // Start countdown at 3 seconds
bool isPauseResumeEnabled = true;  // Default to enabled

// Default timer values (in seconds)
const int defaultWaterTimer = 120;  // 2 minutes - <-----adjust desired time here----->
const int defaultSoapTimer = 30;    // 30 seconds -  <-----adjust desired time here----->
const int defaultAirTimer = 120;     // 2 minute 30 seconds -  <-----adjust desired time here----->
const int defaultGripoTimer = 60;   // 1 minute -  <-----adjust desired time here----->



bool isGripoButtonPressed = false;
unsigned long gripoButtonPressTime = 0;
const unsigned long LONG_PRESS_TIME = 2000; // 2 seconds long press for gripoButton to reset the User Password if forgotten
bool isTimeIncrementEnabled = false; // Default is disabled
unsigned long adminPressStart = 0;
bool adminPressActive = false;
bool gripoCountdownActive = false;
unsigned long gripoCountdownStart = 0;
int gripoCountdownValue = 5;
bool gripoButtonHeld = false;



int grossSalesCH1 = 0;  // Sales for Water
int grossSalesCH2 = 0;  // Sales for Soap
int grossSalesCH3 = 0;  // Sales for Air
int grossSalesCH4 = 0;  // Sales for Gripo


// Save and store password when device is turn-off
byte adminPassword = 0; // Default password (0 means not set)
#define EEPROM_SIZE 512 // Define the size of EEPROM (adjust as needed)
int readSalesFromEEPROM(int address); 

// Long press gripoButton for 3 seconds for a full reset
#define LONG_PRESS_TIME 2000 // 2 seconds
#define VERY_LONG_PRESS_TIME 5000 // 3 seconds

const unsigned long inactivityTimeout = 10000; // Timeout in milliseconds (10 seconds)
bool isMainScreen = true; // Indicates if the system is on the main screen
bool isDisplayingMainScreen = false;
int pauseCount[4] = {0, 0, 0, 0}; // Array to track the number of pauses for each output
int maxPauseCount = 2; // Default maximum pause count

// Function to initialize EEPROM
void initializeEEPROM() {
  if (!EEPROM.begin(EEPROM_SIZE)) {
    Serial.println("Failed to initialize EEPROM");
  }
}

// Function to save the pause/resume setting to EEPROM
void savePauseResumeSetting(bool isEnabled) {
    EEPROM.write(PAUSE_RESUME_SETTING_ADDR, isEnabled ? 1 : 0);
    EEPROM.commit(); // Ensure the data is written to EEPROM
}

// Function to load the pause/resume setting from EEPROM
bool loadPauseResumeSetting() {
    //EEPROM.begin(512); // Initialize EEPROM with size (adjust as necessary)
    bool isEnabled = EEPROM.read(PAUSE_RESUME_SETTING_ADDR);
    return isEnabled == 1; // Return true if enabled, false if disabled
}

void configModeCallback(WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode (AP mode)");
  lcd.clear();
  String connectingMsg = "Connecting...";
  int startCol = (20 - connectingMsg.length()) / 2;
  lcd.setCursor(startCol, 1);
  lcd.print(connectingMsg);
}


void activationWebSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("Activation WS Disconnected, retrying soon...");
      break;
    case WStype_CONNECTED:
      Serial.println("Activation WS Connected");
      break;
    case WStype_TEXT: {
      String message = "";
      for (size_t i = 0; i < length; i++) {
        message += (char)payload[i];
      }
      Serial.println("Activation WS message received: " + message);
      StaticJsonDocument<200> doc;
      DeserializationError error = deserializeJson(doc, message);
      if (error) {
        Serial.print("Activation JSON error: ");
        Serial.println(error.c_str());
        return;
      }
      if (doc.containsKey("status")) {
        String status = String(doc["status"]);
        if (status == "Activated") {
          Serial.println("Activation status received.");
          machineActivated = true;
          if (doc.containsKey("vendor_id")) {
            machineVendorId = doc["vendor_id"];
            EEPROM.write(VENDOR_ID_ADDRESS, machineVendorId);
          }
          if (doc.containsKey("license_key")) {
            machineLicenseKey = String(doc["license_key"]);
            EEPROM.put(70, machineLicenseKey);
          }
          EEPROM.write(ACTIVATION_FLAG_ADDRESS, 1);
          EEPROM.commit();
          // Immediately display the activation message:
          lcd.clear();
          lcd.setCursor((20 - String("Machine Activated!").length()) / 2, 1);
          lcd.print("Machine Activated!");
          // Instead of blocking delay(5000), schedule the transition:
          activationMessageExpiry = millis() + 5000; // show for 5 seconds
          activationMessageType = "activated";
          Serial.println("Machine activated successfully via Activation WS.");
        }
        else if (status == "Available") {
          Serial.println("Revocation status received.");
          machineActivated = false;
          EEPROM.write(ACTIVATION_FLAG_ADDRESS, 0);
          EEPROM.commit();
          lcd.clear();
          lcd.setCursor((20 - String("Machine Deactivated").length()) / 2, 1);
          lcd.print("Machine Deactivated");
          activationMessageExpiry = millis() + 5000;
          activationMessageType = "deactivated";
          Serial.println("Machine revoked successfully via Activation WS.");
        }
        else {
          Serial.println("Received unknown activation status: " + status);
        }
      } else {
        Serial.println("Activation WS received message, but no status field found.");
      }
      break;
    }
    case WStype_PING:
      Serial.println("Activation WS Ping received");
      break;
    case WStype_PONG:
      Serial.println("Activation WS Pong received");
      break;
    default:
      break;
  }
}

void statusWebSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("Status WS Disconnected");
      break;
    case WStype_CONNECTED:
      Serial.println("Status WS Connected");
      break;
    case WStype_TEXT: {
      String message = "";
      for (size_t i = 0; i < length; i++) {
        message += (char)payload[i];
      }
      Serial.println("Status WS message received: " + message);
      // Process any status feedback if needed.
      break;
    }
    case WStype_PING:
      Serial.println("Status WS Ping received");
      break;
    case WStype_PONG:
      Serial.println("Status WS Pong received");
      break;
    default:
      break;
  }
}

void connectActivationWebSocket() {
  Serial.print("Connecting to Activation WS: wss://");
  Serial.print(ws_server);
  Serial.println(ws_path);
  activationWebSocket.beginSSL(ws_server, ws_port, ws_path);
  activationWebSocket.onEvent(activationWebSocketEvent);
}

void connectStatusWebSocket() {
  // If the machine is activated and a vendor id exists, update the URL.
  if (machineActivated && machineVendorId != 0) {
    ws_status_path_str = "/ws/machine-status/" + String(machineVendorId) + "/";
  }
  Serial.print("Connecting to Status WS: wss://");
  Serial.print(ws_server);
  Serial.println(ws_status_path_str);
  
  statusWebSocket.beginSSL(ws_server, ws_port, ws_status_path_str.c_str());
  statusWebSocket.onEvent(statusWebSocketEvent);
}

void sendMachineStatus() {
  if (statusWebSocket.isConnected()) {
    String machineId = getMachineId();
    String payload = "{\"machine_id\": \"" + machineId + "\", \"status\": \"online\"}";
    statusWebSocket.sendTXT(payload);
    Serial.println("Sent machine status: " + payload);
  } else {
    Serial.println("Status WS not connected. Cannot send machine status.");
  }
}


void setup() {
  WiFi.mode(WIFI_STA);
  Serial.begin(115200);
  Wire.begin();
  lcd.begin(20, 4);
  lcd.setBacklight(255);
  initializeEEPROM(); 
  EEPROM.begin(EEPROM_SIZE);
  machineActivated = (EEPROM.read(ACTIVATION_FLAG_ADDRESS) == 1);
  machineVendorId = EEPROM.read(VENDOR_ID_ADDRESS);

  if (machineActivated) {
    // If the machine is activated, connect to the existing WiFi settings.
    connectToInternetSettings();
  }


  connectActivationWebSocket();
  connectStatusWebSocket();
  //if (!machineActivated) {
  //  connectWebSocket();
  //} else {
  //  Serial.println("Machine already activated, skipping WebSocket connection.");
  //}
  if (!machineActivated) {
    lcd.clear();
    lcd.setCursor((20 - 15) / 2, 1); // Center text 
    lcd.print("Activate Machine");
    // Optionally, halt further processing or run a loop waiting for activation
  } else {
    DisplayMainScreen();
  }

  char savedLine1[21];
  EEPROM.get(DISPLAY_NAME_ADDRESS, savedLine1);
  strncpy(line1, savedLine1, sizeof(line1));
  
  // Load or initialize timers from EEPROM
  if (EEPROM.read(waterTimerAddress) == 255) {
    saveTimerToEEPROM(waterTimerAddress, defaultWaterTimer);
  } else {
    waterSwitchIncrement = readTimerFromEEPROM(waterTimerAddress);
  }
  
  if (EEPROM.read(soapTimerAddress) == 255) {
    saveTimerToEEPROM(soapTimerAddress, defaultSoapTimer);
  } else {
    soapSwitchIncrement = readTimerFromEEPROM(soapTimerAddress);
  }
  
  if (EEPROM.read(airTimerAddress) == 255) {
    saveTimerToEEPROM(airTimerAddress, defaultAirTimer);
  } else {
    airSwitchIncrement = readTimerFromEEPROM(airTimerAddress);
  }
  
  if (EEPROM.read(gripoTimerAddress) == 255) {
    saveTimerToEEPROM(gripoTimerAddress, defaultGripoTimer);
  } else {
    gripoSwitchIncrement = readTimerFromEEPROM(gripoTimerAddress);
  }
  
  // Initialize sales values from EEPROM
  grossSalesCH1 = readSalesFromEEPROM(CH1SalesAddress);
  grossSalesCH2 = readSalesFromEEPROM(CH2SalesAddress);
  grossSalesCH3 = readSalesFromEEPROM(CH3SalesAddress);
  grossSalesCH4 = readSalesFromEEPROM(CH4SalesAddress);
  
  // Ensure gross sales values are valid (handle uninitialized EEPROM)
  if (grossSalesCH1 == 255) grossSalesCH1 = 0;
  if (grossSalesCH2 == 255) grossSalesCH2 = 0;
  if (grossSalesCH3 == 255) grossSalesCH3 = 0;
  if (grossSalesCH4 == 255) grossSalesCH4 = 0;
  
  // Set up LCD buttons, outputs, and indicators (do not reinitialize LCD to preserve the message)
  lcd.clear();
  for (int i = 0; i < 4; i++) {
    pinMode(buttonPins[i], INPUT_PULLUP);
    coinSlotPrevState = digitalRead(coinSlot);
    pinMode(outputPins[i], OUTPUT);
    pinMode(indicatorPins[i], OUTPUT);
    digitalWrite(indicatorPins[i], LOW);
    digitalWrite(outputPins[i], LOW);
  }
  
  // Set up coin slot
  pinMode(coinSlot, INPUT_PULLUP);
  
  // Load other settings
  setupAdminPassword();
  DisplayMainScreen();
  isTimeIncrementEnabled = readTimeIncrementFromEEPROM();
  isPauseResumeEnabled = loadPauseResumeSetting(); // Load setting from EEPROM
  readPauseCountFromEEPROM();

}



void loop() {
  unsigned long now = millis();
  handleAdminMode();
  handleNormalOperation();
  handleGripoButtonLongPressNonBlocking();
  updateGripoCountdown();
  BlinkInsertCoins();
  updateIndicators();
  savePauseResumeSetting(isPauseResumeEnabled);

  // Process network tasks on a separate (non-blocking) schedule
  if (now - lastNetworkCheck >= NETWORK_INTERVAL) {
    activationWebSocket.loop();
    statusWebSocket.loop();
    lastNetworkCheck = now;
  }

  if (activationMessageExpiry != 0 && now >= activationMessageExpiry) {
    activationMessageExpiry = 0;
    DisplayMainScreen();
  }
  
  // Periodically send machine status using the status WebSocket.
  if (machineActivated && machineVendorId != 0 && statusWebSocket.isConnected() && 
      (now - lastStatusSend >= statusInterval)) {
    sendMachineStatus();
    lastStatusSend = now;
  }
  
  // Handle countdown display updates.
  if (isCountdownActive && (now - lastCountdownUpdate >= 1000)) {
    lastCountdownUpdate = now;
    countdownSeconds--;
    if (countdownSeconds <= 0) {
      isCountdownActive = false;
      displayAmtPaid(pulse, 0);  // Return to normal display
    } else {
      displayAmtPaid(pulse, 0);  // Update countdown display
    }
  }
  
  if (now - lastTransactionQueueProcess >= TRANSACTION_PROCESS_INTERVAL) {
    processTransactionQueue();
    lastTransactionQueueProcess = now;
  }

  checkAndSendPendingTransactions();
  
  // Check if a system reset is scheduled and execute it.
  if (resetScheduled && now >= resetScheduledTime) {
    ESP.restart();  // For ESP32, you can also use esp_restart();
  }
}


void connectToInternetSettings() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Connecting to");
  lcd.setCursor(0, 1);
  lcd.print("Internet...");
  
  // Create a WiFiManager instance and attempt connection.
  WiFiManager wm;
  wm.setAPCallback(configModeCallback);
  bool res = wm.autoConnect("ChromeCarwash", "chrometech"); 
  if (res) {
      lcd.clear();
      lcd.setCursor(0, 1);
      lcd.print("Connected!");
      // Optionally, store a flag in EEPROM to remember this setting.
      delay(2000);
  } else {
      lcd.clear();
      lcd.setCursor(0, 1);
      lcd.print("Conn Failed");
      delay(2000);
  }
  // return to main screen if successfull.
  DisplayMainScreen();
}

void DisplayMainScreen() {
  lcd.clear();
  lcd.setCursor((20 - strlen(line1)) / 2, 0); // Center the first line
  lcd.print(line1);
  lcd.setCursor((20 - strlen(line2)) / 2, 1); // Center the second line
  lcd.print(line2);
  lcd.setCursor((20 - strlen(line3)) / 2, 2); // Center the third line
  lcd.print(line3);
  isDisplayingMainScreen = true; // Set the state to true
}

void updateLCDText(uint8_t row, const char* text) {
  // This function writes text to the given row.
  // It pads with spaces to avoid leftover characters.
  char buffer[21];
  memset(buffer, ' ', 20);
  buffer[20] = '\0';
  strncpy(buffer, text, min(strlen(text), (size_t)20));
  lcd.setCursor(0, row);
  lcd.print(buffer);
}

void BlinkInsertCoins() {
  if (!isDisplayingMainScreen) return; // Only blink when main screen is active

  unsigned long currentMillis = millis();
  if (currentMillis - coinspreviousMillis >= interval) {
    coinspreviousMillis = currentMillis;
    showInsertCoins = !showInsertCoins; // Toggle the visibility
  }

  lcd.setCursor(0, 3);
  if (showInsertCoins) {
    lcd.print("--- Insert Coins ---");
  } else {
    lcd.print("                    "); // Clear the line
  }
}



void handleAdminMode() {
  // Only allow admin mode when no feature is active.
  for (int i = 0; i < 4; i++) {
    if (outputs[i]) {
      adminPressActive = false; // ensure flag is reset
      return;
    }
  }
  
  // Use a dedicated variable for admin detection:
  if (digitalRead(waterButton) == LOW) {
    if (!adminPressActive) {
      adminPressActive = true;
      adminPressStart = millis();
    } else if (millis() - adminPressStart >= 2000) {  // 2-second hold reached
      adminPressActive = false; // reset flag once triggered
      if (adminPassword == 0) {
        setupAdminPassword();
      } else {
        promptAdminLogin();
      }
    }
  } else {
    adminPressActive = false;
  }
}




void enterAdminMode() {
  inAdminMode = true;
  //selectedFeature = 0; // Default to the first feature
  lastInteractionTime = millis();
  DisplayAdminSettings();
}

void handleNormalOperation() {
    checkCoinSlot(); // Check if a coin has been inserted

    // Handle the button presses for each service only if there is enough credit
    waterSwitch();
    soapSwitch();
    airSwitch();
    gripoSwitch();

    updateTimers(); // Update individual timers for water, soap, air, and gripo

    bool anyTimerRunning = false;
    bool allTimersFinished = true; // Flag to check if all timers are finished

    // Check if any timers are running
    for (int i = 0; i < 4; i++) {
        if (remainingTime[i] > 0) {
            anyTimerRunning = true;
            allTimersFinished = false; // There is still at least one active timer
            break; // Exit the loop early since we found an active timer
        }
    }
    if (updateDisplay && (millis() - displayTimestamp >= displayDelay)) {
      updateDisplay = false;
      DisplayMainScreen();
    }
    // If credits are available, show the remaining time for active services
    if (totalCredits > 0) {
        isMainScreenDisplayed = false;
        isDisplayingMainScreen = false;
        displayRemainingTime();
    } 
    // If no credits but at least one timer is running, keep displaying the remaining time
    else if (totalCredits == 0 && anyTimerRunning) {
        isMainScreenDisplayed = false;
        displayRemainingTime(); // Continue displaying remaining time until all timers finish
    } 
    // If no credits and no timers are running, go back to the main screen **only once**
    else if (totalCredits == 0 && allTimersFinished) {
        if (!isMainScreenDisplayed) {  
            DisplayMainScreen();
            isMainScreenDisplayed = true; 
        }
        timerFinishedStartTime = 0; 
    }

    if (totalCredits == 0 && allTimersFinished && isAnyButtonPressed()) {
        isMainScreenDisplayed = false; 
    }
}

void handleButton1LongPress() {
  if (digitalRead(waterButton) == LOW) {
    if (!isButtonPressed) {
      isButtonPressed = true;
      lastInteractionTime = millis();
    }
    if (millis() - lastInteractionTime >= 2000) {
      isButtonPressed = false;
      if (adminPassword == 0) {
        setupAdminPassword();
      } else {
        promptAdminLogin();
      }
    }
  } else {
    isButtonPressed = false;
  }
}

void waitForButtonRelease() {
  while (digitalRead(waterButton) == LOW || digitalRead(soapButton) == LOW ||
         digitalRead(airButton) == LOW || digitalRead(gripoButton) == LOW) {
    delay(10);
  }
}

// Helper function: flush any residual pressed state on admin buttons.
void flushAdminButtons() {
  while (digitalRead(waterButton) == LOW || digitalRead(soapButton) == LOW ||
         digitalRead(airButton) == LOW || digitalRead(gripoButton) == LOW) {
    delay(10);
  }
  delay(100); // Allow extra settling time
}

// Rebuilt DisplayAdminSettings() function – follows original logic but flushes buttons
void DisplayAdminSettings() {
  // Wait until all buttons are released.
  waitForButtonRelease();
  // Flush any residual LOW state (especially for the soap button)
  flushAdminButtons();

  // Record the time when admin mode is entered.
  unsigned long adminMenuEntryTime = millis();
  // Set initial ignore period and debounce delay.
  const unsigned long initialIgnoreTime = 500; 
  const unsigned long debounceDelay = 300;

  // Initialize debounce timers to current time.
  unsigned long lastWaterPress = millis();
  unsigned long lastSoapPress = millis();
  unsigned long lastGripoPress = millis();
  
  unsigned long lastInteractionTime = millis();
  bool updateScreen = true;

  int currentOption = 1;
  const int mainMaxOptions = 3;
  bool inExtendedMenu = false;

  const int totalExtendedOptions = 6;
  String extendedOptions[totalExtendedOptions] = {
    "Activate Machine",
    "Time Increment",
    "Change Display",
    "Change Password",
    "Connect to WiFi",
    "Update Machine"
  };
  int currentExtendedIndex = 0;
  const int itemsPerPage = 3;

  int previousOption = -1;
  int previousExtendedIndex = -1;

  while (true) {
    unsigned long now = millis();
    // Inactivity timeout: exit to main screen if no interaction.
    if (now - lastInteractionTime > INACTIVITY_TIMEOUT) {
      DisplayMainScreen();
      return;
    }

    // Update the LCD display when the option changes.
    if (!inExtendedMenu) {
      if (currentOption != previousOption || updateScreen) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Admin Settings:");
        lcd.setCursor(0, 1);
        lcd.print((currentOption == 1) ? "> Adjust Timers" : "  Adjust Timers");
        lcd.setCursor(0, 2);
        lcd.print((currentOption == 2) ? "> Gross Sales" : "  Gross Sales");
        lcd.setCursor(0, 3);
        lcd.print((currentOption == 3) ? "> Pause/Resume" : "  Pause/Resume");
        previousOption = currentOption;
        updateScreen = false;
      }
    } else {
      int currentPage = currentExtendedIndex / itemsPerPage;
      if (currentExtendedIndex != previousExtendedIndex || updateScreen) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Extended Settings:");
        for (int i = 0; i < itemsPerPage; i++) {
          int optionIndex = currentPage * itemsPerPage + i;
          lcd.setCursor(0, i + 1);
          if (optionIndex < totalExtendedOptions) {
            if (optionIndex == currentExtendedIndex)
              lcd.print("> " + extendedOptions[optionIndex]);
            else
              lcd.print("  " + extendedOptions[optionIndex]);
          } else {
            lcd.print("                ");
          }
        }
        previousExtendedIndex = currentExtendedIndex;
        updateScreen = false;
      }
    }

    // Only process button events after the initial ignore period.
    if (now - adminMenuEntryTime > initialIgnoreTime) {
      // Check waterButton: cycle options (or move forward in extended menu)
      if (digitalRead(waterButton) == LOW && (now - lastWaterPress >= debounceDelay)) {
        lastWaterPress = now;
        lastInteractionTime = now;
        if (!inExtendedMenu) {
          currentOption++;
          if (currentOption > mainMaxOptions) {
            inExtendedMenu = true;
            currentExtendedIndex = 0;
            previousExtendedIndex = -1;
          }
        } else {
          currentExtendedIndex = (currentExtendedIndex + 1) % totalExtendedOptions;
        }
        updateScreen = true;
      }

      // Check soapButton: select the current option.
      if (digitalRead(soapButton) == LOW && (now - lastSoapPress >= debounceDelay)) {
        lastSoapPress = now;
        lastInteractionTime = now;
        // Flush buttons right before entering the submenu.
        flushAdminButtons();
        if (!inExtendedMenu) {
          switch (currentOption) {
            case 1:
              adjustTimers();      // This function should also begin with flushAdminButtons()
              break;
            case 2:
              viewGrossSales();    // Flush at start of function
              break;
            case 3:
              toggleFeaturePauseMenu(); // Flush at start
              break;
          }
        } else {
          switch (currentExtendedIndex) {
            case 0:
              activateMachineSettings(); // Flush at start
              break;
            case 1:
              adjustTimeIncrement();       // Flush at start
              break;
            case 2:
              changeDisplayName();         // Flush at start
              break;
            case 3:
              resetAdminPassword();        // Flush at start
              break;
            case 4:
              connectToInternetSettings(); // Flush at start if needed
              break;
            case 5:
              updateMachine();
              break;
          }
        }
        // After executing the submenu, exit the admin settings menu.
        return;
      }

      // Check gripoButton: go back in the menu or exit to main screen.
      if (digitalRead(gripoButton) == LOW && (now - lastGripoPress >= debounceDelay)) {
        lastGripoPress = now;
        lastInteractionTime = now;
        if (inExtendedMenu) {
          if (currentExtendedIndex == 0) {
            inExtendedMenu = false;
            currentOption = 1;
            previousOption = -1; // Force screen update.
          } else {
            currentExtendedIndex--;
          }
        } else {
          DisplayMainScreen();
          return;
        }
        updateScreen = true;
      }
    }
    delay(10); // Yield a short delay.
  }
}


void setupAdminPassword() {
  lcd.clear();
  int selectedNumber = 1;
  unsigned long gripoButtonPressTime = 0;
  bool isGripoButtonPressed = false;    

  EEPROM.get(0, adminPassword);
  if (adminPassword > 0 && adminPassword <= 20) {

    DisplayMainScreen();
    return;
  }

  while (true) {
    lcd.setCursor(0, 0);
    lcd.print("Set New Password");
    lcd.setCursor(0, 1);
    lcd.print("Password: ");
    lcd.print(selectedNumber);
    lcd.print("       ");

    if (digitalRead(gripoButton) == LOW) {
      if (!isGripoButtonPressed) {
        gripoButtonPressTime = millis();
        isGripoButtonPressed = true;
      }

      unsigned long pressDuration = millis() - gripoButtonPressTime;

      if (pressDuration >= VERY_LONG_PRESS_TIME) {
        resetSystem(); 
        return;
      } else if (pressDuration >= LONG_PRESS_TIME) {
        adminPassword = 0;
        EEPROM.put(0, adminPassword); 
        EEPROM.commit();
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Password Reset");
        lcd.setCursor(0, 1);
        lcd.print("Set New Password");
        delay(2000);
        break; 
      }
    } else {
      isGripoButtonPressed = false; 
    }

    if (digitalRead(waterButton) == LOW) {
      delay(300); 
      selectedNumber++;
      if (selectedNumber > 20) selectedNumber = 1;
    }

    if (digitalRead(soapButton) == LOW) {
      delay(300); // Debounce delay
      adminPassword = selectedNumber;
      EEPROM.put(0, adminPassword);
      EEPROM.commit();
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Password Set: ");
      lcd.print(adminPassword);
      delay(2000);
      blinkIndicators(3, 500); 
      DisplayMainScreen();
      return;
    }
  }
}

void promptAdminLogin() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Enter Admin Password");
  int enteredPassword = 1;

  EEPROM.get(0, adminPassword);

  while (true) {
    lcd.setCursor(0, 1);
    lcd.print("Password: ");
    lcd.print(enteredPassword);
    lcd.print("       "); 

    if (digitalRead(waterButton) == LOW) {
      delay(300); 
      enteredPassword++;
      if (enteredPassword > 20) enteredPassword = 1;
    }

    if (digitalRead(soapButton) == LOW) {
      delay(300); 
      if (enteredPassword == adminPassword) {
        lcd.setCursor(0, 2);
        lcd.print("Access Granted");
        delay(1000);
        DisplayAdminSettings();
        return;
      } else {
        lcd.setCursor(0, 2);
        lcd.print("Invalid Password!");
        delay(2000);
        DisplayMainScreen();
        return;
      }
    }
  }
}

void resetAdminPassword() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Resetting Password");
  delay(1000);

  adminPassword = 0;
  EEPROM.put(0, adminPassword);
  EEPROM.commit();

  lcd.setCursor(0, 1);
  lcd.print("Password Cleared");
  delay(2000);

  blinkIndicators(3, 500); 

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Set New Password");
  delay(2000);

  setupAdminPassword(); 
}


bool readTimeIncrementFromEEPROM() {
    byte savedValue = EEPROM.read(timeIncrementAddress); 
    return (savedValue == 1);                           
}

void writeTimeIncrementToEEPROM(bool value) {
    EEPROM.write(timeIncrementAddress, value ? 1 : 0); 
    EEPROM.commit();                                   
}

void adjustTimeIncrement() {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Time Increment:");

    int currentOption = isTimeIncrementEnabled ? 1 : 2; 
    unsigned long lastInteractionTime = millis();

    while (true) {
        if (millis() - lastInteractionTime > INACTIVITY_TIMEOUT) {
            DisplayAdminSettings();
            return;
        }

        // Display the current selection
        lcd.setCursor(0, 1);
        if (currentOption == 1) {
            lcd.print("> Enable      ");
            lcd.setCursor(0, 2);
            lcd.print("  Disable     ");
        } else {
            lcd.print("  Enable      ");
            lcd.setCursor(0, 2);
            lcd.print("> Disable     ");
        }

        if (digitalRead(waterButton) == LOW) {
            delay(300);
            lastInteractionTime = millis(); 
            currentOption = (currentOption == 1) ? 2 : 1; 
        }


        if (digitalRead(soapButton) == LOW) {
            delay(300); 
            lastInteractionTime = millis();

            if (currentOption == 1) {
                isTimeIncrementEnabled = true;
                lcd.clear();
                lcd.setCursor(0, 0);
                lcd.print("Increment: Enabled");
                blinkIndicators(3, 500);
            } else {
                isTimeIncrementEnabled = false;
                lcd.clear();
                lcd.setCursor(0, 0);
                lcd.print("Increment: Disabled");
                blinkIndicators(3, 500);
            }

            writeTimeIncrementToEEPROM(isTimeIncrementEnabled);

            delay(2000); 
            DisplayAdminSettings();
            return; 
        }

        if (digitalRead(gripoButton) == LOW) {
            delay(300);
            return;
        }
    }
}

void adjustTimers() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Adjust Timers:");

  int selectedTimer = 0; 

  int timerValues[] = {
    waterSwitchIncrement,
    soapSwitchIncrement,
    airSwitchIncrement,
    gripoSwitchIncrement
  };

  const char* timerNames[] = {"Water", "Soap", "Air", "Gripo"};
  unsigned long lastInteractionTime = millis();
  int salesPerIncrement = 5;

  while (true) {
    if (millis() - lastInteractionTime > INACTIVITY_TIMEOUT) {
      DisplayAdminSettings();
      return;
    }
    lcd.setCursor(0, 1);
    lcd.print("Adjust ");
    lcd.print(timerNames[selectedTimer]);
    lcd.setCursor(0, 2);
    lcd.print("Time: ");
    lcd.print(timerValues[selectedTimer]);
    lcd.print(" sec ");

    if (digitalRead(waterButton) == LOW) {
      delay(300);
      lastInteractionTime = millis(); 
      selectedTimer = (selectedTimer + 1) % 4; 
      lcd.clear();
    }

    if (digitalRead(soapButton) == LOW) {
      delay(300); 
      lastInteractionTime = millis();
      timerValues[selectedTimer] += salesPerIncrement;
      if (timerValues[selectedTimer] > 300) timerValues[selectedTimer] = 5;

      saveTimerToEEPROM(getTimerAddress(selectedTimer), timerValues[selectedTimer]);
    }

    if (digitalRead(airButton) == LOW) {
      delay(300); 
      lastInteractionTime = millis();
      timerValues[selectedTimer] -= salesPerIncrement;
      if (timerValues[selectedTimer] < 5) timerValues[selectedTimer] = 300;

      saveTimerToEEPROM(getTimerAddress(selectedTimer), timerValues[selectedTimer]);
    }

    if (digitalRead(gripoButton) == LOW) { 
      delay(300); 
      lastInteractionTime = millis(); 

      // Save the updated timers
      handleTimerUpdates(timerValues);

      // Display confirmation message
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Timers Updated!");
      blinkIndicators(3, 500);
      delay(1000);

      // Return to admin settings
      DisplayAdminSettings();
      return;
    }
  }
}

int getTimerAddress(int timerIndex) {
  switch (timerIndex) {
    case 0: return waterTimerAddress;
    case 1: return soapTimerAddress;
    case 2: return airTimerAddress;
    case 3: return gripoTimerAddress;
    default: return -1; // Invalid index
  }
}

void handleTimerUpdates(int timerValues[]) {
  // Update the time increments for each function
  waterSwitchIncrement = timerValues[0];
  soapSwitchIncrement = timerValues[1];
  airSwitchIncrement = timerValues[2];
  gripoSwitchIncrement = timerValues[3];

  // Save the timer values to EEPROM
  saveTimerToEEPROM(waterTimerAddress, timerValues[0]);
  saveTimerToEEPROM(soapTimerAddress, timerValues[1]);
  saveTimerToEEPROM(airTimerAddress, timerValues[2]);
  saveTimerToEEPROM(gripoTimerAddress, timerValues[3]);
}

void saveTimerToEEPROM(int address, int value) {
  EEPROM.write(address, value); // Store the value in EEPROM (use more advanced methods for multi-byte values)
  EEPROM.commit();
}

int readTimerFromEEPROM(int address) {
  return EEPROM.read(address); // Retrieve the value from EEPROM
}

void toggleFeaturePauseMenu() {
  lcd.clear();

  // Load the current state from EEPROM
  isPauseResumeEnabled = readPauseResumeStateFromEEPROM();

  int currentOption = isPauseResumeEnabled ? 1 : 2; // 1 for Enable, 2 for Disable
  unsigned long lastInteractionTime = millis();
  int previousOption = 0; // To track changes in the menu option

  while (true) {
    // Handle inactivity timeout
    if (millis() - lastInteractionTime > INACTIVITY_TIMEOUT) {
      DisplayAdminSettings(); // Go back to the main settings menu
      return;
    }

    // Update the display only if the option has changed
    if (currentOption != previousOption) {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Pause/Resume Menu:");
      lcd.setCursor(0, 1);
      lcd.print((currentOption == 1) ? "> Enable" : "  Enable");
      lcd.setCursor(0, 2);
      lcd.print((currentOption == 2) ? "> Disable" : "  Disable");
      lcd.setCursor(0, 3);
      lcd.print((currentOption == 3) ? "> Pause Count" : "  Pause Count");
      previousOption = currentOption; // Update the previous option
    }

    // Toggle options when waterButton is pressed
    if (digitalRead(waterButton) == LOW) { // Toggle between options
      delay(300); // Debounce delay
      lastInteractionTime = millis(); // Reset inactivity timer
      currentOption++;
      if (currentOption > 3) currentOption = 1; // Loop back to the first option
    }

if (digitalRead(soapButton) == LOW) { // Confirm selection
      delay(300); // Debounce delay
      lastInteractionTime = millis(); // Reset inactivity timer

      if (currentOption == 1) {
        // Enable Pause/Resume
        isPauseResumeEnabled = true;
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Timer Pause Enabled");
        blinkIndicators(3, 500);
      } else if (currentOption == 2) {
        // Disable Pause/Resume
        isPauseResumeEnabled = false;
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Timer Pause Disabled");
        blinkIndicators(3, 500);
      } else if (currentOption == 3) {
        // Set Pause Count
        setPauseCount();
      }

      // Save the selected state to EEPROM
      writePauseResumeStateToEEPROM(isPauseResumeEnabled);

      delay(1000); // Display confirmation message
      DisplayAdminSettings(); // Return to admin menu
      return;
    }
  }
}

void setPauseCount() {
  lcd.clear();
  int selectedCount = maxPauseCount;
  unsigned long lastButtonPressTime = 0; // For debouncing
  unsigned long lastInteractionTime = millis();
  char buffer[20]; // Buffer for formatted string

  while (true) {
    // Handle inactivity timeout
    if (millis() - lastInteractionTime > INACTIVITY_TIMEOUT) {
      DisplayAdminSettings();
      return;
    }

    lcd.setCursor(0, 0);
    lcd.print("Set Pause Count");
    lcd.setCursor(0, 1);
    sprintf(buffer, "Pause Count: %d", selectedCount);
    lcd.print(buffer);
    lcd.print("   "); // Clear trailing characters

    // Scroll numbers with waterButton
    if (digitalRead(waterButton) == LOW && millis() - lastButtonPressTime > debounceTime) {
      lastButtonPressTime = millis(); // Update last button press time
      lastInteractionTime = millis(); // Reset inactivity timer
      selectedCount++;
      if (selectedCount > 5) selectedCount = 1;
    }

    // Confirm pause count selection with soapButton
    if (digitalRead(soapButton) == LOW && millis() - lastButtonPressTime > debounceTime) {
      lastButtonPressTime = millis(); // Update last button press time
      lastInteractionTime = millis(); // Reset inactivity timer
      maxPauseCount = selectedCount;

      // Save the selected count to EEPROM
      EEPROM.write(51, maxPauseCount);
      EEPROM.commit();

      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Pause Count Set!");
      lcd.setCursor(0, 1);
      sprintf(buffer, "Pause Count: %d", maxPauseCount);
      lcd.print(buffer);
      blinkIndicators(3, 500);

      unsigned long confirmationStartTime = millis();
      while (millis() - confirmationStartTime < 2000) {
        // Wait for 2 seconds
      }
      return;
    }

    // Go back to the previous menu using gripoButton
    if (digitalRead(gripoButton) == LOW && millis() - lastButtonPressTime > debounceTime) {
      lastButtonPressTime = millis(); // Update last button press time
      return; // Exit to the previous menu
    }
  }
}

void readPauseCountFromEEPROM() {
  maxPauseCount = EEPROM.read(51);
  if (maxPauseCount == 255) { // Handle uninitialized EEPROM value
    maxPauseCount = 2; // Default value
  }
}

void enableFeature() {
  // Implement the logic to enable the feature
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Feature Enabled");
  delay(2000);
}

void disableFeature() {
  // Implement the logic to disable the feature
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Feature Disabled");
  delay(2000);
}

// Read the pause/resume state from EEPROM
bool readPauseResumeStateFromEEPROM() {
    byte savedState = EEPROM.read(pauseResumeStateAddress);
    return (savedState == 1); // Return true if 1, false otherwise
}

// Write the pause/resume state to EEPROM
void writePauseResumeStateToEEPROM(bool state) {
    EEPROM.write(pauseResumeStateAddress, state ? 1 : 0); // Write 1 for true, 0 for false
    EEPROM.commit(); // Commit changes to EEPROM
}

// Function to check the coin slot and handle coin insertion
void checkCoinSlot() {
  unsigned long now = millis();
  if (now - lastCoinMillis > debounceTime) {
    int currentStatus = digitalRead(coinSlot);
    // Detect a falling edge (transition from HIGH to LOW)
    if (currentStatus == LOW && coinSlotPrevState == HIGH) {
      pulse++;  
      totalCredits++;
      coinInserted = true;
      
      // Clear the display to remove any residual main screen text
      lcd.clear();
      isDisplayingMainScreen = false;
      
      // Update the display with the new credit value.
      char creditsStr[21];
      sprintf(creditsStr, "Credits: %d.0", pulse);
      updateLCDText(0, creditsStr);
      
      lastCoinMillis = now;
    }
    coinSlotPrevState = currentStatus;
  }
}






float getTimeIncrement(int index) {
    switch (index) {
        case 0: return waterSwitchIncrement;
        case 1: return soapSwitchIncrement;
        case 2: return airSwitchIncrement;
        case 3: return gripoSwitchIncrement;
        default: return 0;
    }
}

bool isAnyTimerActive() {
  for (int i = 0; i < 4; i++) {
    if (remainingTime[i] > 0) {
      return true;
    }
  }
  return false;
}

// Prevents unnecessary flickering of the display
void displayAmtPaid(int pulse, int creditsNeeded) { 
    // Update credits without always clearing the timer area.
    // Instead of clearing the entire display, only update row 0.
    char lcdBuffer[21];
    // Update row 0 with credits.
    sprintf(lcdBuffer, "Credits: %d.0", pulse);
    lcd.setCursor(0, 0);  
    lcd.print(lcdBuffer);
    
    // Update row 1 with a message if additional coins are needed.
    if (creditsNeeded > 0) {
        sprintf(lcdBuffer, "Add %d Peso Coins", creditsNeeded);
        updateLCDText(1, lcdBuffer);
    } else {
        // If no additional coins are needed, clear row 1.
        updateLCDText(1, "                    ");
        // Only update the timer display if a timer is active.
        if (pulse >= 5 && isAnyTimerActive()) {  
            displayRemainingTime();
        }
    }
}



void updateIndicators() {
  bool anyOutputHigh = false;
  static unsigned long lastBlinkTime = 0;
  static bool blinkState = false;

  // First pass: Check if any output is high
  for (int i = 0; i < 4; i++) {
    if (outputs[i]) {
      anyOutputHigh = true;
      break;
    }
  }

  // Second pass: Handle indicators for all outputs
  for (int i = 0; i < 4; i++) {
    if (outputs[i]) {
      if (remainingTime[i] <= 10) {
        // Blink the indicator if remaining time is 10 seconds or less
        unsigned long currentMillis = millis();
        if (currentMillis - lastBlinkTime >= 500) { // Sync with seconds
          lastBlinkTime = currentMillis;
          blinkState = !blinkState;
        }
        digitalWrite(indicatorPins[i], blinkState ? HIGH : LOW);
      } else {
        digitalWrite(indicatorPins[i], LOW); // Turn off indicator if the specific feature is turned on
      }
    } else {
      if (i == 3) { // Gripo
        if (totalCredits >= 1) {
          digitalWrite(indicatorPins[i], HIGH); // Turn on indicator if credit is sufficient for Gripo
        } else {
          digitalWrite(indicatorPins[i], LOW); // Turn off indicator otherwise
        }
      } else { // Water, Soap, Air
        if (totalCredits >= 5) {
          digitalWrite(indicatorPins[i], HIGH); // Turn on indicator if credit is sufficient
        } else {
          digitalWrite(indicatorPins[i], LOW); // Turn off indicator otherwise
        }
      }
    }
  }
}

void blinkIndicators(int times, int interval) {
  for (int i = 0; i < times; i++) {
    for (int j = 0; j < 4; j++) {
      digitalWrite(indicatorPins[j], HIGH);
    }
    delay(interval);
    for (int j = 0; j < 4; j++) {
      digitalWrite(indicatorPins[j], LOW);
    }
    delay(interval);
  }
}

void processTransactionQueue() {
  Transaction tx;
  if (dequeueTransaction(tx)) {
    sendTransactionToBackend(tx.index, tx.amount);
  }
}

void scheduleTransaction(int index, int amountAddedToSales) {
  // Enqueue the transaction using your existing queue.
  if (!enqueueTransaction(index, amountAddedToSales)) {
    Serial.println("Transaction queue full, unable to schedule transaction.");
    // Optionally, handle queue overflow here.
  }
}


void handleSwitch(int buttonPin, int outputPin, int index, float timeIncrement) {
    static int pauseCount[4] = {0, 0, 0, 0};  // Track pause count per feature
    static unsigned long lastButtonPressTime[4] = {0, 0, 0, 0};
    unsigned long currentTime = millis();
    if (buttonPin == waterButton && adminPressActive && pulse == 0) {
        return;
    }
    if (digitalRead(buttonPin) == LOW) {
        if (currentTime - lastButtonPress > 200) {  // Global debounce
            lastInteractionTime = currentTime;
            lastButtonPress = currentTime;

            // Handle Pause/Resume Logic first
            if (outputs[index] && isPauseResumeEnabled) {
                if (isPaused[index]) {
                    isPaused[index] = false;  // Resume operation
                    digitalWrite(outputPin, HIGH);
                } else if (pauseCount[index] < maxPauseCount) {
                    isPaused[index] = true;   // Pause the feature
                    digitalWrite(outputPin, LOW);
                    pauseCount[index]++;
                }
                return;
            }

            // Check if there are enough credits
            int requiredCredits = (index == 3) ? 1 : 5;
            if (pulse >= requiredCredits) {
                int usedCredits = (pulse / requiredCredits) * requiredCredits;
                pulse -= usedCredits;
                totalCredits -= usedCredits;
                remainingTime[index] += (usedCredits / requiredCredits) * timeIncrement;

                // Start this channel's timer immediately
                previousMillis[index] = currentTime;

                // Activate feature
                outputs[index] = true;
                digitalWrite(outputPin, HIGH);
                digitalWrite(indicatorPins[index], LOW);
                activeFeature[index] = true;
                pauseCount[index] = 0;  // Reset pause count

                int amountAddedToSales = usedCredits;
                // Update sales accordingly
                switch (index) {
                    case 0:
                        grossSalesCH1 += amountAddedToSales;
                        writeSalesToEEPROM(CH1SalesAddress, grossSalesCH1);
                        break;
                    case 1:
                        grossSalesCH2 += amountAddedToSales;
                        writeSalesToEEPROM(CH2SalesAddress, grossSalesCH2);
                        break;
                    case 2:
                        grossSalesCH3 += amountAddedToSales;
                        writeSalesToEEPROM(CH3SalesAddress, grossSalesCH3);
                        break;
                    case 3:
                        grossSalesCH4 += amountAddedToSales;
                        writeSalesToEEPROM(CH4SalesAddress, grossSalesCH4);
                        break;
                }

                // Instead of sending now, store pending transaction info
                pendingTransactionAmount[index] = amountAddedToSales;
                transactionSent[index] = false;
                
                // Immediately update the display.
                lcd.clear();
                displayAmtPaid(pulse, 0);
                lastButtonPressTime[index] = currentTime;

                if (pulse == 0) {
                    lcd.setCursor(0, 1);
                    lcd.print("                    ");
                }
            } else {
                int creditsNeeded = requiredCredits - pulse;
                displayAmtPaid(pulse, creditsNeeded);
            }
        }
    }
    
    // Inactivity timeout check.
    if (millis() - lastInteractionTime >= inactivityTimeout && !isMainScreen) {
        lcd.clear();
        isMainScreen = true;
    }
}




void waterSwitch() {
    handleSwitch(waterButton, waterOutput, 0, waterSwitchIncrement);
}

void soapSwitch() {
    handleSwitch(soapButton, soapOutput, 1, soapSwitchIncrement);
}

void airSwitch() {
    handleSwitch(airButton, airOutput, 2, airSwitchIncrement);
}

void gripoSwitch() {
    handleSwitch(gripoButton, gripoOutput, 3, gripoSwitchIncrement);
}


void updateTimer(int index, int outputPin) {
    unsigned long currentMillis = millis();
    if (isPaused[index]) {
        // When paused, reset the timer’s reference time so that no extra seconds are subtracted.
        previousMillis[index] = currentMillis;
        return;
    }
    if (currentMillis - previousMillis[index] >= 1000) {
        unsigned long elapsedSeconds = (currentMillis - previousMillis[index]) / 1000;
        previousMillis[index] += elapsedSeconds * 1000;
        if (outputs[index] && remainingTime[index] > 0.0) {
            remainingTime[index] -= elapsedSeconds;
        }
        if (remainingTime[index] <= 0.0) {
            remainingTime[index] = 0.0;
            outputs[index] = false;
            digitalWrite(outputPin, LOW);
            if (totalCredits >= 5) digitalWrite(indicatorPins[index], HIGH);
            pauseCount[index] = 0;
            // Do not send transaction here.
        }
    }
}

void checkAndSendPendingTransactions() {
    bool allTimersFinished = true;
    for (int i = 0; i < 4; i++) {
        if (remainingTime[i] > 0) {
            allTimersFinished = false;
            break;
        }
    }
    if (allTimersFinished) {
        for (int i = 0; i < 4; i++) {
            if (!transactionSent[i] && pendingTransactionAmount[i] > 0) {
                scheduleTransaction(i, pendingTransactionAmount[i]);
                transactionSent[i] = true;
                pendingTransactionAmount[i] = 0;
            }
        }
    }
}






void updateTimers() {
    updateTimer(0, waterOutput);
    updateTimer(1, soapOutput);
    updateTimer(2, airOutput);
    updateTimer(3, gripoOutput);
}

void displayRemainingTime() {
    static unsigned long blinkTimer = 0; // For blinking interval
    static bool blinkState = true;        // To toggle blink state

    unsigned long currentMillis = millis();

    // Iterate over all timers to check if any are still running
    for (int i = 0; i < 4; i++) {
        int minutes = 0;
        int seconds = 0;

        if (remainingTime[i] > 0) {
            minutes = remainingTime[i] / 60;
            seconds = (int)remainingTime[i] % 60;
        }

        int column = (i < 2) ? 0 : 11; // Left or right column
        int row = (i % 2 == 0) ? 2 : 3; // Top or bottom row

        lcd.setCursor(column, row);

        if (isPaused[i]) { // If the timer is paused
            if (currentMillis - blinkTimer >= 1000) { // Toggle blink state every 1 second
                blinkTimer = currentMillis;
                blinkState = !blinkState;
            }

            if (blinkState) {
                sprintf(lcdBuffer, "CH%d:%02d:%02d", i + 1, minutes, seconds);
                lcd.print(lcdBuffer);
            } else {
                lcd.print("         "); // Clear the display during blink off
            }
        } else { // If the timer is running
            sprintf(lcdBuffer, "CH%d:%02d:%02d", i + 1, minutes, seconds);
            lcd.print(lcdBuffer);
        }
    }
}

void readDisplayNameFromEEPROM() {
  char savedLine1[21];
  EEPROM.get(DISPLAY_NAME_ADDRESS, savedLine1);
  strncpy(line1, savedLine1, sizeof(line1));
}

void writeDisplayNameToEEPROM(const char* newName) {
  EEPROM.put(DISPLAY_NAME_ADDRESS, newName);
  EEPROM.commit();
}

void viewGrossSales() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Gross Sales:");

  char buffer[20]; // Buffer for formatted string
  unsigned long gripoButtonPressTime = 0;
  bool isGripoButtonPressed = false;
  unsigned long lastAirButtonPressTime = 0;
  const unsigned long debounceDelay = 1; // Debounce delay in milliseconds

  while (true) {
    // Read sales values from EEPROM
    int grossSalesCH1 = readSalesFromEEPROM(CH1SalesAddress);
    int grossSalesCH2 = readSalesFromEEPROM(CH2SalesAddress);
    int grossSalesCH3 = readSalesFromEEPROM(CH3SalesAddress);
    int grossSalesCH4 = readSalesFromEEPROM(CH4SalesAddress);

    // Handle uninitialized values (0xFFFF)
    if (grossSalesCH1 == 0xFFFF) grossSalesCH1 = 0; // EEPROM returns 0xFFFF if uninitialized
    if (grossSalesCH2 == 0xFFFF) grossSalesCH2 = 0;
    if (grossSalesCH3 == 0xFFFF) grossSalesCH3 = 0;
    if (grossSalesCH4 == 0xFFFF) grossSalesCH4 = 0;

    // Calculate total sales
    int totalSales = grossSalesCH1 + grossSalesCH2 + grossSalesCH3 + grossSalesCH4;

    // Display sales
    lcd.setCursor(0, 1);
    lcd.print("Water: ");
    lcd.print(grossSalesCH1); 
    lcd.setCursor(12, 1);
    lcd.print("Air: ");
    lcd.print(grossSalesCH3);
    lcd.setCursor(0, 2);
    lcd.print("Soap: ");
    lcd.print(grossSalesCH2); 
    lcd.setCursor(0, 3);
    lcd.print("Gripo: ");
    lcd.print(grossSalesCH4); 

    // Display total sales
    lcd.setCursor(0, 0); 
    sprintf(buffer, "Total Sales: %d", totalSales);
    lcd.print(buffer);

    delay(5000); // Wait before the next update

    // Check if the Gripo button is pressed
    if (digitalRead(gripoButton) == LOW) {
      if (!isGripoButtonPressed) {
        gripoButtonPressTime = millis();
        isGripoButtonPressed = true;
      }

      unsigned long pressDuration = millis() - gripoButtonPressTime;

      if (pressDuration >= 5000) { // Long press detected
        resetSalesWithConfirmation(); // Call the confirmation-based reset function
        DisplayAdminSettings();
        return;
      }
    } else if (isGripoButtonPressed) {
      // Short press detected
      isGripoButtonPressed = false;
    }

    // Check if the air button is pressed to go back to admin settings
    if (digitalRead(airButton) == LOW) {
      unsigned long currentMillis = millis();
      if (currentMillis - lastAirButtonPressTime > debounceDelay) {
        lastAirButtonPressTime = currentMillis;
        DisplayAdminSettings();
        return;
      }
    }
  }
}

void writeSalesToEEPROM(int address, int value) {
    EEPROM.put(address, value);
    EEPROM.commit();
}

int readSalesFromEEPROM(int address) {
    int value;
    EEPROM.get(address, value);
    return value;
}

void batchUpdateSales(int water, int soap, int air, int gripo) {
    writeSalesToEEPROM(CH1SalesAddress, water);
    writeSalesToEEPROM(CH2SalesAddress, soap);
    writeSalesToEEPROM(CH3SalesAddress, air);
    writeSalesToEEPROM(CH4SalesAddress, gripo);
    EEPROM.commit(); // Commit sales changes once for all updates
}

void resetSales() {
    // Reset sales values in memory
    grossSalesCH1 = 0;
    grossSalesCH2 = 0;
    grossSalesCH3 = 0;
    grossSalesCH4 = 0;

    // Write 0 to all sales addresses in EEPROM
    writeSalesToEEPROM(CH1SalesAddress, 0);
    writeSalesToEEPROM(CH2SalesAddress, 0);
    writeSalesToEEPROM(CH3SalesAddress, 0);
    writeSalesToEEPROM(CH4SalesAddress, 0);

    // Commit the changes to EEPROM
    EEPROM.commit();
}


void resetSalesWithConfirmation() {
  unsigned long now = millis();
  
  switch(resetSalesState) {
    case RS_DONE:
      // Initialize the confirmation screen once.
      lcd.clear();
      updateLCDText(0, "Confirm Reset?");
      updateLCDText(1, "Press CH-1 to Reset");
      resetSalesStartTime = now;
      resetSalesState = RS_WAIT_CONFIRM;
      break;
      
    case RS_WAIT_CONFIRM:
      // Check for button press (waterButton used to confirm)
      if (digitalRead(waterButton) == LOW) {
        // Debounce: only accept if 300ms have passed since last press
        if (now - resetSalesDebounceTime >= 300) {
          resetSalesDebounceTime = now;
          resetSalesState = RS_CONFIRMED;
        }
      }
      // If 5 seconds pass without confirmation, cancel the reset.
      if (now - resetSalesStartTime >= 5000) {
        resetSalesState = RS_CANCELLED;
        resetSalesStartTime = now;  // re-use start time for display duration
      }
      break;
      
    case RS_CONFIRMED:
      // Perform the resetSales operation.
      resetSales();
      lcd.clear();
      updateLCDText(0, "All Sales Reset");
      // (Optionally, you could schedule a blinking message here)
      // Set state to done after 2 seconds.
      resetSalesStartTime = now;
      resetSalesState = RS_DONE;
      break;
      
    case RS_CANCELLED:
      lcd.clear();
      updateLCDText(0, "Reset Cancelled");
      // After showing the cancellation message for 2 seconds, return to admin menu.
      if (now - resetSalesStartTime >= 2000) {
        resetSalesState = RS_DONE;
        // Return to admin settings screen:
        DisplayAdminSettings();
      }
      break;
      
    default:
      break;
  }
}

void handleGripoButtonLongPressNonBlocking() {
  unsigned long currentMillis = millis();
  // Check if the gripo button is pressed
  if (digitalRead(gripoButton) == LOW) {
    if (!gripoButtonHeld) {
      gripoButtonHeld = true;
      gripoButtonPressTime = currentMillis;
    }
    // If held longer than VERY_LONG_PRESS_TIME, start the countdown if not already started
    if (!gripoCountdownActive && (currentMillis - gripoButtonPressTime >= VERY_LONG_PRESS_TIME)) {
      gripoCountdownActive = true;
      gripoCountdownStart = currentMillis;
      gripoCountdownValue = 5;
    }
  } else {
    // Reset the state if the button is released
    gripoButtonHeld = false;
    gripoCountdownActive = false;
  }
}

void updateGripoCountdown() {
  if (gripoCountdownActive) {
    unsigned long now = millis();
    if (now - gripoCountdownStart >= 1000) {  // update every second
      gripoCountdownStart = now;
      gripoCountdownValue--;
      char countStr[21];
      sprintf(countStr, "Reset in: %d", gripoCountdownValue);
      updateLCDText(0, countStr);  // update first line
      if (gripoCountdownValue <= 0) {
        gripoCountdownActive = false;
        resetSystem(); // Call your system reset function
      }
    }
  }
}

// Add this function to reset the entire system and EEPROM
void resetSystem() {
  // Update the LCD to inform the user
  lcd.clear();
  lcd.setCursor(0, 1);
  lcd.print("Resetting System...");
  // Schedule the actual reset 1000 ms from now.
  resetScheduledTime = millis() + 1000;
  resetScheduled = true;
}

void changeDisplayName() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Change Display Name");

  String newLine1 = "";
  char currentChar = 'a';
  unsigned long lastPressTime = 0;
  const unsigned long debounceDelay = 300;
  bool blinkState = false; // Tracks the blinking state
  unsigned long lastBlinkTime = 0;
  const unsigned long blinkInterval = 1000; // Blink every 1 second

  while (true) {
    // Display the current line
    lcd.setCursor(0, 1);
    lcd.print("Line 1: ");
    lcd.setCursor(0, 2);
    lcd.print(newLine1);

    // Clear the previous cursor position and redraw
    lcd.setCursor(newLine1.length(), 2);
    if (blinkState) {
      lcd.print("|"); // Show the blinking cursor
    } else {
      lcd.print(" "); // Hide the blinking cursor
    }

    // Blink cursor logic
    unsigned long currentMillis = millis();
    if (currentMillis - lastBlinkTime >= blinkInterval) {
      blinkState = !blinkState; // Toggle blink state
      lastBlinkTime = currentMillis;
    }

    // Display "Select:" and the current character
    lcd.setCursor(0, 3);
    lcd.print("Select: ");
    lcd.print(currentChar);

    // Scroll letters with waterButton
    if (digitalRead(waterButton) == LOW) {
      delay(debounceDelay); // Debounce
      currentChar++;
      if (currentChar > 'z') currentChar = 'a'; // Loop back to 'a' after 'z'
    }

    // Confirm the selected letter with soapButton
    if (digitalRead(soapButton) == LOW) {
      newLine1 += currentChar; // Add the current character to the line
      currentChar = 'a';       // Reset to 'a' after confirming the letter
      delay(debounceDelay);    // Debounce
    }

    // Erase letters with airButton (single press)
    if (digitalRead(airButton) == LOW) {
      if (newLine1.length() > 0) {
        // Erase the last character
        newLine1.remove(newLine1.length() - 1);

        // Clear the erased character's position
        lcd.setCursor(newLine1.length(), 2);
        lcd.print(" "); // Clear the position of the erased character

        // Ensure the cursor moves to the correct position
        lcd.setCursor(newLine1.length(), 2);
      }
      delay(debounceDelay); // Debounce
    }

    // Add space with gripoButton (single press)
    if (digitalRead(gripoButton) == LOW) {
      newLine1 += ' '; // Add a space to the line
      delay(debounceDelay); // Debounce
    }

    // Save settings with gripoButton (long press)
    if (digitalRead(gripoButton) == LOW) {
      currentMillis = millis();
      if (currentMillis - lastPressTime > 1000) { // Long press detected
        // Save the new display name
        strncpy(line1, newLine1.c_str(), sizeof(line1));
        EEPROM.put(DISPLAY_NAME_ADDRESS, line1);
        EEPROM.commit();

        // Display success message
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Change Screen");
        lcd.setCursor(0, 1);
        lcd.print("Successfully Changed!");
        blinkIndicators(5, 200);

        // Return to the main screen
        DisplayMainScreen();
        return;
      }
      lastPressTime = millis();
    }
  }
}



int parseVendorId(String json) {
  int vendorId = 0;
  int keyIndex = json.indexOf("\"vendor_id\"");
  if (keyIndex != -1) {
    int colonIndex = json.indexOf(":", keyIndex);
    if (colonIndex != -1) {
      int i = colonIndex + 1;
      // Skip any whitespace
      while (json.charAt(i) == ' ') {
        i++;
      }
      // Build a string of digits.
      String numStr = "";
      while (i < json.length() && isDigit(json.charAt(i))) {
        numStr += json.charAt(i);
        i++;
      }
      vendorId = numStr.toInt();
    }
  }
  return vendorId;
}

// Function to retrieve the machine's unique identifier (using MAC address)
String getMachineId() {
  String mac = WiFi.macAddress();  // e.g. "AA:BB:CC:DD:EE:FF"
  mac.replace(":", "");            // e.g. "AABBCCDDEEFF"
  return mac;
}

void activateMachine(String licenseKey, String machineId) {
  const char* licenseBackendUrl = "https://carwash-api.chrometechnology.shop/api/activate-license/";
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, cannot activate license.");
    lcd.clear();
    // Use a centralized LCD update function (assumed defined elsewhere)
    updateLCDText(1, "WiFi Not Conn.");
    activationMessageExpiry = millis() + 2000; // schedule transition in 2 sec
    return;
  }
  
  HTTPClient http;
  http.begin(licenseBackendUrl);
  http.addHeader("Content-Type", "application/json");

  String payload = "{\"license_key\": \"" + licenseKey + "\", \"machine_id\": \"" + machineId + "\"}";
  int httpResponseCode = http.POST(payload);
  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.println("License activation response: " + response);
    if (response.indexOf("\"status\":\"Activated\"") != -1) {
      int id = parseVendorId(response);
      machineVendorId = id;
      machineActivated = true;
      EEPROM.write(ACTIVATION_FLAG_ADDRESS, 1);
      EEPROM.write(VENDOR_ID_ADDRESS, machineVendorId);
      EEPROM.commit();
      lcd.clear();
      updateLCDText(1, "Machine Activated!");
      activationMessageExpiry = millis() + 2000;
    } else {
      lcd.clear();
      updateLCDText(1, "Invalid License Key");
      machineActivated = false;
      EEPROM.write(ACTIVATION_FLAG_ADDRESS, 0);
      EEPROM.commit();
      activationMessageExpiry = millis() + 2000;
    }
  } else {
    lcd.clear();
    updateLCDText(1, "Activation Failed");
    Serial.println("License activation failed: " + http.errorToString(httpResponseCode));
    activationMessageExpiry = millis() + 2000;
  }
  http.end();
}



void activateMachineSettings() {
  // Retrieve the unique machine ID for display.
  String machineId = getMachineId();
  
  // If the machine is already activated, show the activation info and then return to the main screen.
  if (machineActivated) {
    lcd.clear();
    lcd.setCursor(0, 1);
    lcd.print("Status: Activated");
    lcd.setCursor(0, 2);
    lcd.print("Machine ID:");
    lcd.setCursor(0, 3);
    lcd.print(machineId);
    delay(5000);  // Display for 5 seconds
    DisplayMainScreen();
    return;
  }
  
  // Otherwise, display the activation menu (machine not activated yet)
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Activate - Press CH2");
  lcd.setCursor(0, 1);
  lcd.print("Status: Not Activated");
  lcd.setCursor(0, 2);
  lcd.print("Machine ID:");
  lcd.setCursor(0, 3);
  lcd.print(machineId);

  while (true) {
    // If an activation (e.g., via WebSocket) is received while waiting,
    // show the activation message for 5 seconds then return to main screen.
    if (machineActivated) {
      lcd.clear();
      lcd.setCursor((20 - String("Machine Activated!").length()) / 2, 1);
      lcd.print("Machine Activated!");
      Serial.println("Activation received via WebSocket.");
      delay(5000);
      DisplayMainScreen();
      break;
    }
    
    if (digitalRead(soapButton) == LOW) {
      delay(300);
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Enter License Key:");
      lcd.setCursor(0, 2);
      lcd.print("Select: ");
      
      String licenseKey = "";
      char currentChar = '0';
      const unsigned long debounceDelay = 300;
      bool cursorBlink = false;
      unsigned long lastBlinkTime = millis();
      const unsigned long blinkInterval = 1000;
      
      while (true) {
        lcd.setCursor(0, 1);
        lcd.print(licenseKey);
        int cursorPos, cursorRow;
        if (licenseKey.length() < 19) {
          cursorPos = licenseKey.length();
          cursorRow = 1;
        } else {
          cursorPos = 0;
          cursorRow = 3;
        }
        lcd.setCursor(cursorPos, cursorRow);
        unsigned long currentMillis = millis();
        if (currentMillis - lastBlinkTime >= blinkInterval) {
          cursorBlink = !cursorBlink;
          lastBlinkTime = currentMillis;
        }
        lcd.print(cursorBlink ? "|" : " ");
        
        lcd.setCursor(8, 2);
        lcd.print(currentChar);
        
        if (digitalRead(waterButton) == LOW) {  
          delay(debounceDelay);
          if (currentChar == '9')
            currentChar = 'A';
          else if (currentChar == 'Z')
            currentChar = '0';
          else
            currentChar++;
        }
        
        if (digitalRead(soapButton) == LOW) {  
          delay(debounceDelay);
          licenseKey += currentChar;
        }
        
        if (digitalRead(airButton) == LOW) {  
          delay(debounceDelay);
          if (licenseKey.length() > 0)
            licenseKey.remove(licenseKey.length() - 1);
        }
        
        if (digitalRead(gripoButton) == LOW) { 
          delay(debounceDelay);
          lcd.clear();
          lcd.setCursor(0, 1);
          lcd.print("Activating...");
          activateMachine(licenseKey, machineId);
          break;
        }
        delay(50);
      }
      
      // After a manual activation attempt, refresh the activation menu display.
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Activate");
      lcd.setCursor(0, 1);
      lcd.print(machineActivated ? "Status: Activated" : "Status: Not Activated");
      lcd.setCursor(0, 2);
      lcd.print("Machine ID:");
      lcd.setCursor(0, 3);
      lcd.print(machineId);
      delay(5000);  // Show updated status for 5 seconds
      DisplayMainScreen();
      break;
    }
    
    // If the GRIPO button is pressed, exit back to the admin settings.
    if (digitalRead(gripoButton) == LOW) {
      delay(300);
      DisplayAdminSettings();
      break;
    }
    
    delay(100);
  }
}

void showWelcomeMessage() {
  String machineId = getMachineId();
  
  HTTPClient http;
  http.begin("https://carwash-api.chrometechnology.shop/licenses/"); // GET all licenses
  int httpResponseCode = http.GET();
  
  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.println("Licenses response: " + response);
    
    // Simple search: if our machineId is found in the response,
    // we try to extract the value for "transferred_to"
    if (response.indexOf(machineId) > -1) {
      int idx = response.indexOf("\"transferred_to\":");
      if (idx > -1) {
        idx += strlen("\"transferred_to\":");
        // Skip whitespace and any starting quotes
        while (response.charAt(idx) == ' ' || response.charAt(idx) == '\"') {
          idx++;
        }
        int endIdx = response.indexOf("\"", idx);
        String username = response.substring(idx, endIdx);
        // Capitalize first letter if needed.
        username[0] = toupper(username[0]);
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Welcome Back,");
        lcd.setCursor(0, 1);
        lcd.print(username + "!");
        http.end();
        return;
      }
    }
    // Fallback if not found:
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Welcome Back,");
    lcd.setCursor(0, 1);
    lcd.print("User!");
  } else {
    Serial.println("GET licenses failed: " + http.errorToString(httpResponseCode));
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Welcome Back,");
    lcd.setCursor(0, 1);
    lcd.print("User!");
  }
  http.end();
}

void sendTransactionToBackend(int serviceIndex, int amount) {
  String machineId = getMachineId();
  if (!machineActivated) {
    Serial.println("Machine not activated. Please activate machine.");
    //lcd.clear();
    //lcd.setCursor((20 - 15) / 2, 1);
    //lcd.print("Activate Machine");
    return;
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, cannot send transaction.");
    return;
  }

  HTTPClient http;
  http.begin(backendUrl); // Your backend endpoint URL for transactions
  http.addHeader("Content-Type", "application/json");

  // Compute overall total earnings by summing sales for all channels
  int totalEarnings = grossSalesCH1 + grossSalesCH2 + grossSalesCH3 + grossSalesCH4;

  // Payment method is "Coin", QRCode Payment Integration(soon)
  String paymentMethod = "Coin";

  // Use the vendor id stored from activation
  int vendorId = machineVendorId;

  // Build the JSON payload with all required fields, using the stored machineLicenseKey.
  String payload = "{";
  payload += "\"vendor\": " + String(vendorId) + ", ";
  payload += "\"service\": \"" + String(serviceNames[serviceIndex]) + "\", ";
  payload += "\"amount\": " + String((float)amount, 2) + ", ";
  payload += "\"total_water\": \"" + String((float)grossSalesCH1, 2) + "\", ";
  payload += "\"total_soap\": \"" + String((float)grossSalesCH2, 2) + "\", ";
  payload += "\"total_air\": \"" + String((float)grossSalesCH3, 2) + "\", ";
  payload += "\"total_gripo\": \"" + String((float)grossSalesCH4, 2) + "\", ";
  payload += "\"earnings\": " + String((float)totalEarnings, 2) + ", ";
  payload += "\"payment\": \"" + paymentMethod + "\", ";
  payload += "\"license_key\": \"" + machineLicenseKey + "\", ";
  payload += "\"machine_id\": \"" + machineId + "\"";
  payload += "}";

  int httpResponseCode = http.POST(payload);
  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.println("Transaction sent successfully: " + response);
  } else {
    Serial.println("Error sending transaction: " + http.errorToString(httpResponseCode));
  }
  http.end();
}

// Helper function to query GitHub API for the latest firmware URL
String getLatestFirmwareURL() {
  String apiUrl = "https://api.github.com/repos/chrometech8/4in1-carwash-vendo/releases/latest";
  String firmwareUrl = "";
  
  WiFiClientSecure client;
  client.setInsecure();  // Disable certificate validation
  HTTPClient http;
  
  http.begin(client, apiUrl);
  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, payload);
    if (!error) {
      // Loop through the assets array to find the asset ending with ".merged.bin"
      JsonArray assets = doc["assets"].as<JsonArray>();
      for (JsonObject asset : assets) {
        String name = asset["name"].as<String>();
        if (name.endsWith(".merged.bin")) {
          firmwareUrl = asset["browser_download_url"].as<String>();
          break;
        }
      }
    } else {
      Serial.println("JSON parse error in getLatestFirmwareURL");
    }
  } else {
    Serial.printf("GitHub API HTTP error: %d\n", httpCode);
  }
  http.end();
  return firmwareUrl;
}

void updateMachine() {
  // Use static variables to track state
  static bool updateStarted = false;
  static unsigned long updateStartTime = 0;
  static String latestFirmwareUrl = "";
  
  // On the first call, fetch the latest firmware URL and start the update
  if (!updateStarted) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Fetching Update...");
    
    latestFirmwareUrl = getLatestFirmwareURL();
    if (latestFirmwareUrl == "") {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("No Update Info");
      updateStartTime = millis();
      updateStarted = true;
      return;
    }
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Updating...");
    
    // Create a secure client for HTTPS and disable certificate validation
    WiFiClientSecure client;
    client.setInsecure();
    
    // Instantiate HTTPUpdate and perform the update using the URL from GitHub API
    HTTPUpdate httpUpdate;
    HTTPUpdateResult ret = httpUpdate.update(client, latestFirmwareUrl.c_str());
    
    switch(ret) {
      case HTTP_UPDATE_FAILED:
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Update Failed");
        Serial.printf("Update failed: %s\n", httpUpdate.getLastErrorString().c_str());
        break;
      case HTTP_UPDATE_NO_UPDATES:
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("No Update");
        break;
      case HTTP_UPDATE_OK:
        // On success, the ESP32 will reboot automatically.
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Update Success");
        break;
    }
    
    updateStartTime = millis();
    updateStarted = true;
  }
  // On subsequent calls, wait non-blockingly until 2000ms have elapsed before returning to admin settings.
  else {
    if (millis() - updateStartTime >= 2000) {
      updateStarted = false;  // Reset update state for future updates
      DisplayAdminSettings();
    }
  }
}





