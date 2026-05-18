
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include "web_handler.h"

#define SERVICE_UUID        "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define DIGITAL_CHAR_UUID   "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
#define AUTH_CHAR_UUID      "6e400004-b5a3-f393-e0a9-e50e24dcca9e"
#define STATUS_CHAR_UUID    "6e400005-b5a3-f393-e0a9-e50e24dcca9e"

#define AUTH_TIMEOUT_MS  30000

#define WIFI_SSID        "INTELLI_PLC"
#define WIFI_PASSWORD    "12345678"

#define WIFI_RETRY_INTERVAL_MS  10000

User        users[MAX_USERS];
int         userCount    = 0;
Preferences prefs;
WebServer   webServer(80);

// Output pin lookup table — order must match the web UI dropdowns (index 0-4)
const OutPin OUTPUT_PINS[NUM_OUTPUT_PINS] = {
  { "R0_0", R0_0 },
  { "Q0_0", Q0_0 },
  { "Q0_1", Q0_1 },
  { "Q0_2", Q0_2 },
  { "Q0_3", Q0_3 }
};

// Current output-to-motion mapping (indices into OUTPUT_PINS[])
// Defaults match the legacy hardcoded assignments
uint8_t outIdxEstop = 0;  // R0_0
uint8_t outIdxUp    = 1;  // Q0_0
uint8_t outIdxDown  = 2;  // Q0_1

// Write to a configured output only when a real pin is assigned.
// outIdxXxx == NUM_OUTPUT_PINS means "None" — skip the write.
#define DOUT(idx, val) \
  do { if ((idx) < NUM_OUTPUT_PINS) digitalWrite(OUTPUT_PINS[(idx)].pin, (val)); } while(0)

static void forceAllOutputsOff() {
  motionEstop = true;   // emergency mode: E-STOP output LOW
  motionUp    = false;
  motionDown  = false;
  for (int i = 0; i < NUM_OUTPUT_PINS; i++) {
    digitalWrite(OUTPUT_PINS[i].pin, LOW);
  }
}

bool motionEstop = true;
bool motionUp    = false;
bool motionDown  = false;

bool          wifiConnected  = false;
unsigned long wifiRetryTime  = 0;

// BLE
BLEServer         *pBLEServer    = nullptr;
BLECharacteristic *digitalChar   = nullptr;
BLECharacteristic *authChar      = nullptr;
BLECharacteristic *statusChar    = nullptr;

bool     deviceConnected  = false;
bool     authenticated    = false;
char     connectedUserEmail[64] = "";
uint16_t connId           = 0;        
unsigned long connectTime = 0;


void saveUsers() {
  prefs.begin("bleusers", false);
  prefs.putInt("count", userCount);
  for (int i = 0; i < userCount; i++) {
    char ekey[12], pkey[12], nkey[12], rkey[12];
    sprintf(ekey, "email_%d", i);
    sprintf(pkey, "pass_%d",  i);
    sprintf(nkey, "name_%d",  i);
    sprintf(rkey, "role_%d",  i);
    prefs.putString(ekey, users[i].email);
    prefs.putString(pkey, users[i].password);
    prefs.putString(nkey, users[i].name);
    prefs.putString(rkey, users[i].role);
  }
  prefs.end();
}

void loadUsers() {
  prefs.begin("bleusers", true);
  userCount = prefs.getInt("count", 0);
  for (int i = 0; i < userCount && i < MAX_USERS; i++) {
    char ekey[12], pkey[12], nkey[12], rkey[12];
    sprintf(ekey, "email_%d", i);
    sprintf(pkey, "pass_%d",  i);
    sprintf(nkey, "name_%d",  i);
    sprintf(rkey, "role_%d",  i);
    prefs.getString(ekey, users[i].email,    sizeof(users[i].email));
    prefs.getString(pkey, users[i].password, sizeof(users[i].password));
    prefs.getString(nkey, users[i].name,     sizeof(users[i].name));
    prefs.getString(rkey, users[i].role,     sizeof(users[i].role));
    // backward compat: if name/role empty, fill defaults
    if (users[i].name[0] == '\0') strncpy(users[i].name, users[i].email, sizeof(users[i].name));
    if (users[i].role[0] == '\0') strncpy(users[i].role, "Operator",     sizeof(users[i].role));
  }
  prefs.end();

  // Always ensure at least one Admin user exists in the list.
  // Handles both fresh NVS (userCount==0) and stale NVS from older firmware
  // that did not include the admin@plc.com account.
  bool hasAdmin = false;
  for (int i = 0; i < userCount; i++) {
    if (String(users[i].role) == "Admin") { hasAdmin = true; break; }
  }
  if (!hasAdmin && userCount < MAX_USERS) {
    strncpy(users[userCount].name,     "Admin",         sizeof(users[0].name));
    strncpy(users[userCount].email,    "admin@plc.com", sizeof(users[0].email));
    strncpy(users[userCount].password, "Admin123",      sizeof(users[0].password));
    strncpy(users[userCount].role,     "Admin",         sizeof(users[0].role));
    userCount++;
    saveUsers();
    Serial.println("Default admin user created/restored (admin@plc.com / Admin123)");
  }
}

// Sets all 5 output GPIOs to OUTPUT mode.
// Default runtime state keeps E-STOP output ON (unless emergency is active).
// Called on startup and whenever the configuration is changed.
void applyOutputConfig() {
  for (int i = 0; i < NUM_OUTPUT_PINS; i++) {
    pinMode(OUTPUT_PINS[i].pin, OUTPUT);
    digitalWrite(OUTPUT_PINS[i].pin, LOW);
  }
  DOUT(outIdxEstop, motionEstop ? LOW : HIGH);
}

// Reads output-to-motion pin indices from NVS namespace "outconfig".
// Falls back to the legacy defaults if the key is missing or out of range.
void loadOutputConfig() {
  prefs.begin("outconfig", true);
  int e = prefs.getInt("estop_idx", 0);
  int u = prefs.getInt("up_idx",    1);
  int d = prefs.getInt("down_idx",  2);
  prefs.end();
  // Allow 0..NUM_OUTPUT_PINS (NUM_OUTPUT_PINS = "None" / unassigned)
  outIdxEstop = (e >= 0 && e <= NUM_OUTPUT_PINS) ? (uint8_t)e : 0;
  outIdxUp    = (u >= 0 && u <= NUM_OUTPUT_PINS) ? (uint8_t)u : 1;
  outIdxDown  = (d >= 0 && d <= NUM_OUTPUT_PINS) ? (uint8_t)d : 2;
  applyOutputConfig();
  auto pl = [](uint8_t i) -> const char* {
    return (i < NUM_OUTPUT_PINS) ? OUTPUT_PINS[i].label : "None";
  };
  Serial.printf("Output config: ESTOP=%s UP=%s DOWN=%s\n",
    pl(outIdxEstop), pl(outIdxUp), pl(outIdxDown));
}

// Persists the current indices to NVS and re-initialises pin modes.
void saveOutputConfig() {
  prefs.begin("outconfig", false);
  prefs.putInt("estop_idx", outIdxEstop);
  prefs.putInt("up_idx",    outIdxUp);
  prefs.putInt("down_idx",  outIdxDown);
  prefs.end();
  applyOutputConfig();
}

// Sends current motion state to the mobile app via the status characteristic.
// Format: "ST:[estop],[up],[down]"  e.g. "0,1,0" = UP active
void notifyMotionStatus() {
  if (!deviceConnected || !authenticated || !statusChar) return;
  char buf[16];
  snprintf(buf, sizeof(buf), "%d,%d,%d",
    motionEstop ? 1 : 0,
    motionUp    ? 1 : 0,
    motionDown  ? 1 : 0);
  statusChar->setValue(buf);
  statusChar->notify();
  Serial.printf("BLE status notify: %s\n", buf);
}

bool checkCredentials(const std::string& email, const std::string& password) {
  for (int i = 0; i < userCount; i++) {
    if (email == users[i].email && password == users[i].password) {
      return true;
    }
  }
  return false;
}

void startWiFi() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_SSID, WIFI_PASSWORD);
  delay(100); // allow AP to start
  Serial.println("WiFi AP started");
  Serial.println("SSID: " + String(WIFI_SSID));
  Serial.println("Password: " + String(WIFI_PASSWORD));
  Serial.println("Web panel: http://" + WiFi.softAPIP().toString() + "/");
}

void handleWiFi() {
  // No connection management needed in AP mode
}

// ---------------- SERVER CALLBACK ----------------
class MyServerCallbacks : public BLEServerCallbacks {
  // Extended onConnect gives us the connection handle
  void onConnect(BLEServer* pServer, esp_ble_gatts_cb_param_t *param) {
    deviceConnected = true;
    authenticated   = false;
    connId          = param->connect.conn_id;
    connectTime     = millis();
    Serial.println("Mobile connected — waiting for credentials");

    forceAllOutputsOff();
    Serial.println("Outputs forced OFF until authentication");

    authChar->setValue("AUTH_REQ:email|password");
    authChar->notify();
  }

  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    authenticated   = false;
    connectedUserEmail[0] = '\0';
    Serial.println("Mobile disconnected");

    forceAllOutputsOff();
    Serial.println("All outputs forced OFF due to disconnect");

    delay(100);
    BLEDevice::startAdvertising();
  }
};

class AuthCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    std::string data = pCharacteristic->getValue();
    Serial.printf("Auth data received: %s\n", data.c_str());
    if (data.length() < 3) {
      authChar->setValue("AUTH_FAIL");
      authChar->notify();
      delay(200);
      pBLEServer->disconnect(connId);
      return;
    }

    // Split on '|'
    size_t sep = data.find('|');
    if (sep == std::string::npos) {
      Serial.println("Auth: bad format — expected email|password");
      authChar->setValue("AUTH_FAIL");
      authChar->notify();
      delay(200);
      pBLEServer->disconnect(connId);
      return;
    }

    std::string email    = data.substr(0, sep);
    std::string password = data.substr(sep + 1);

    Serial.printf("Auth attempt — email: %s\n", email.c_str());

    if (checkCredentials(email, password)) {
      authenticated = true;
      strncpy(connectedUserEmail, email.c_str(), sizeof(connectedUserEmail) - 1);
      connectedUserEmail[sizeof(connectedUserEmail) - 1] = '\0';
      authChar->setValue("AUTH_OK");
      authChar->notify();
      Serial.printf("Authentication SUCCESS — user: %s\n", email.c_str());
    } else {
      authenticated = false;
      authChar->setValue("AUTH_FAIL");
      authChar->notify();
      Serial.printf("Authentication FAILED — user: %s\n", email.c_str());
      delay(200);  // allow notify to reach the app before disconnect
      pBLEServer->disconnect(connId);
    }
  }
};

// ---------------- DIGITAL WRITE CALLBACK ----------------
// Expected data format: [estop,up,down]  e.g. "[0,1,0]" or "0,1,0"
// estop=1 → E-STOP output OFF and ALL outputs OFF (emergency stop)
// estop=0 → E-STOP output ON and up/down follow command values
class DigitalCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    // Reject commands from unauthenticated clients
    if (!authenticated) {
      Serial.println("Command rejected — not authenticated");
      return;
    }

    std::string value = pCharacteristic->getValue();
    if (value.length() < 1) return;

    // Parse array: skip leading '[' if present
    const char* str = value.c_str();
    if (*str == '[') str++;

    int estop = 0, up = 0, down = 0;
    int parsed = sscanf(str, "%d,%d,%d", &estop, &up, &down);

    if (parsed < 3) {
      Serial.printf("Parse error: only %d values found in: %s\n", parsed, value.c_str());
      return;
    }

    Serial.printf("Received: [%d,%d,%d]\n", estop, up, down);

    // Safety: UP and DOWN must never be active at the same time
    if (up && down) {
      up   = 0;
      down = 0;
      Serial.println("CONFLICT: UP+DOWN both active — both forced OFF");
    }

    if (estop == 1) {
      // Emergency stop — all outputs OFF (including E-STOP output)
      motionEstop = true;
      motionUp    = false;
      motionDown  = false;
      DOUT(outIdxEstop, LOW);
      DOUT(outIdxUp,    LOW);
      DOUT(outIdxDown,  LOW);
      Serial.println("E-STOP ACTIVE: E-STOP/Others OFF");
    } else {
      // Normal operation
      motionEstop = false;
      motionUp    = (up   != 0);
      motionDown  = (down != 0);
      DOUT(outIdxEstop, HIGH);
      DOUT(outIdxUp,    up   ? HIGH : LOW);
      DOUT(outIdxDown,  down ? HIGH : LOW);
      Serial.printf("E-STOP:ON UP:%d DOWN:%d\n", up, down);
    }
    notifyMotionStatus();
  }
};

void setup() {
  Serial.begin(115200);

  loadUsers();
  loadOutputConfig();  // reads NVS, applies pinMode + LOW for all 5 outputs
  forceAllOutputsOff();

  startWiFi();

  setupWebRoutes();

  BLEDevice::init("PLC14_BLE");

  pBLEServer = BLEDevice::createServer();
  pBLEServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pBLEServer->createService(SERVICE_UUID);

  
  digitalChar = pService->createCharacteristic(
    DIGITAL_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  digitalChar->setCallbacks(new DigitalCallbacks());

  authChar = pService->createCharacteristic(
    AUTH_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY
  );
  authChar->addDescriptor(new BLE2902());
  authChar->setCallbacks(new AuthCallbacks());

  statusChar = pService->createCharacteristic(
    STATUS_CHAR_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  statusChar->addDescriptor(new BLE2902());

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->start();

  Serial.println("BLE Ready...");
}

void loop() {

  if (deviceConnected) {

    if (!authenticated && (millis() - connectTime > AUTH_TIMEOUT_MS)) {
      Serial.println("Auth timeout — disconnecting client");
      authChar->setValue("AUTH_TIMEOUT");
      authChar->notify();
      delay(200);
      pBLEServer->disconnect(connId);
      return;
    }
  } else {
    forceAllOutputsOff();
  }

  handleWiFi(); // does nothing in AP mode
  webServer.handleClient();

  delay(200); 
}