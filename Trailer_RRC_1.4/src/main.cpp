
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
#define HEARTBEAT_CHAR_UUID "6e400006-b5a3-f393-e0a9-e50e24dcca9e"

#define AUTH_TIMEOUT_MS  30000

#define NET_NAME_MIN_LEN 6
#define NET_NAME_MAX_LEN 10
#define WIFI_PASS_MIN_LEN 8
#define WIFI_PASS_MAX_LEN 10

#define DEFAULT_WIFI_SSID "RRC_PLC"
#define DEFAULT_WIFI_PASSWORD  "12345678"
#define DEFAULT_BLE_NAME       "RRC_PLC"

#define WIFI_RETRY_INTERVAL_MS  10000

char bleName[32]       = DEFAULT_BLE_NAME;
char wifiSsid[32]      = DEFAULT_WIFI_SSID;
char wifiPassword[64]  = DEFAULT_WIFI_PASSWORD;

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
uint8_t outIdxLeft  = 3;  // Q0_2
uint8_t outIdxRight = 4;  // Q0_3

// Write to a configured output only when a real pin is assigned.
// outIdxXxx == NUM_OUTPUT_PINS means "None" — skip the write.
#define DOUT(idx, val) \
  do { if ((idx) < NUM_OUTPUT_PINS) digitalWrite(OUTPUT_PINS[(idx)].pin, (val)); } while(0)

static void forceAllOutputsOff() {
  motionEstop = true;   // emergency mode: E-STOP output LOW
  motionUp    = false;
  motionDown  = false;
  motionLeft  = false;
  motionRight = false;
  for (int i = 0; i < NUM_OUTPUT_PINS; i++) {
    digitalWrite(OUTPUT_PINS[i].pin, LOW);
  }
}

bool motionEstop = true;
bool motionUp    = false;
bool motionDown  = false;
bool motionLeft  = false;
bool motionRight = false;

bool          wifiConnected  = false;
unsigned long wifiRetryTime  = 0;

// BLE
BLEServer         *pBLEServer    = nullptr;
BLECharacteristic *digitalChar   = nullptr;
BLECharacteristic *authChar      = nullptr;
BLECharacteristic *statusChar    = nullptr;
BLECharacteristic *heartbeatChar = nullptr;

bool     deviceConnected  = false;
bool     authenticated    = false;
char     connectedUserEmail[64] = "";
uint16_t connId           = 0;        
unsigned long connectTime = 0;

bool          heartbeatAlive     = false;
unsigned long lastHeartbeatTime  = 0;
int           heartbeatMissCount = 3;


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
  int l = prefs.getInt("left_idx",  3);
  int r = prefs.getInt("right_idx", 4);
  prefs.end();

  // Allow 0..NUM_OUTPUT_PINS (NUM_OUTPUT_PINS = "None" / unassigned)
  outIdxEstop = (e >= 0 && e <= NUM_OUTPUT_PINS) ? (uint8_t)e : 0;
  outIdxUp    = (u >= 0 && u <= NUM_OUTPUT_PINS) ? (uint8_t)u : 1;
  outIdxDown  = (d >= 0 && d <= NUM_OUTPUT_PINS) ? (uint8_t)d : 2;
  outIdxLeft  = (l >= 0 && l <= NUM_OUTPUT_PINS) ? (uint8_t)l : 3;
  outIdxRight = (r >= 0 && r <= NUM_OUTPUT_PINS) ? (uint8_t)r : 4;
  applyOutputConfig();
  auto pl = [](uint8_t i) -> const char* {
    return (i < NUM_OUTPUT_PINS) ? OUTPUT_PINS[i].label : "None";
  };
  Serial.printf("Output config: ESTOP=%s UP=%s DOWN=%s LEFT=%s RIGHT=%s\n",
    pl(outIdxEstop), pl(outIdxUp), pl(outIdxDown), pl(outIdxLeft), pl(outIdxRight));
}

// Persists the current indices to NVS and re-initialises pin modes.
void saveOutputConfig() {
  prefs.begin("outconfig", false);
  prefs.putInt("estop_idx", outIdxEstop);
  prefs.putInt("up_idx",    outIdxUp);
  prefs.putInt("down_idx",  outIdxDown);
  prefs.putInt("left_idx",  outIdxLeft);
  prefs.putInt("right_idx", outIdxRight);
  prefs.end();
  applyOutputConfig();
}

// Reads BLE and WiFi credentials from NVS and applies defaults if missing.
void loadNetworkConfig() {
  prefs.begin("netconfig", true);
  String ssid = prefs.getString("wifi_ssid", DEFAULT_WIFI_SSID);
  String pwd  = prefs.getString("wifi_password", DEFAULT_WIFI_PASSWORD);
  String ble  = prefs.getString("ble_name", DEFAULT_BLE_NAME);
  prefs.end();

  if (ssid.length() < NET_NAME_MIN_LEN || ssid.length() > NET_NAME_MAX_LEN) ssid = DEFAULT_WIFI_SSID;
  if (pwd.length() < WIFI_PASS_MIN_LEN || pwd.length() > WIFI_PASS_MAX_LEN) pwd = DEFAULT_WIFI_PASSWORD;
  if (ble.length() < NET_NAME_MIN_LEN || ble.length() > NET_NAME_MAX_LEN || !ble.startsWith("RRC_")) ble = DEFAULT_BLE_NAME;

  if (ssid.length() >= sizeof(wifiSsid)) ssid = ssid.substring(0, sizeof(wifiSsid) - 1);
  if (pwd.length() >= sizeof(wifiPassword)) pwd = pwd.substring(0, sizeof(wifiPassword) - 1);
  if (ble.length() >= sizeof(bleName)) ble = ble.substring(0, sizeof(bleName) - 1);

  ssid.toCharArray(wifiSsid, sizeof(wifiSsid));
  pwd.toCharArray(wifiPassword, sizeof(wifiPassword));
  ble.toCharArray(bleName, sizeof(bleName));
}

void saveNetworkConfig() {
  prefs.begin("netconfig", false);
  prefs.putString("wifi_ssid", wifiSsid);
  prefs.putString("wifi_password", wifiPassword);
  prefs.putString("ble_name", bleName);
  prefs.end();
}

// Sends current motion state to the mobile app via the status characteristic.
// Format: "ST:[estop],[up],[down]"  e.g. "0,1,0" = UP active
void notifyMotionStatus() {
  if (!deviceConnected || !authenticated || !statusChar) return;
  char buf[24];
  snprintf(buf, sizeof(buf), "%d,%d,%d,%d,%d",
    motionEstop ? 1 : 0,
    motionUp    ? 1 : 0,
    motionDown  ? 1 : 0,
    motionLeft  ? 1 : 0,
    motionRight ? 1 : 0);
  statusChar->setValue(buf);
  statusChar->notify();
  Serial.printf("BLE status notify: %s\n", buf);
}

static bool isHeartbeatHealthy() {
  if (!deviceConnected || !authenticated || lastHeartbeatTime == 0) return false;
  return (millis() - lastHeartbeatTime) <= 300;
}

static void updateOutputPinsFromState() {
  if (!deviceConnected || !authenticated || !isHeartbeatHealthy()) {
    for (int i = 0; i < NUM_OUTPUT_PINS; i++) {
      digitalWrite(OUTPUT_PINS[i].pin, LOW);
    }
    return;
  }

  if (motionEstop) {
    for (int i = 0; i < NUM_OUTPUT_PINS; i++) {
      digitalWrite(OUTPUT_PINS[i].pin, LOW);
    }
    return;
  }

  DOUT(outIdxEstop, HIGH);
  DOUT(outIdxUp,    motionUp    ? HIGH : LOW);
  DOUT(outIdxDown,  motionDown  ? HIGH : LOW);
  DOUT(outIdxLeft,  motionLeft  ? HIGH : LOW);
  DOUT(outIdxRight, motionRight ? HIGH : LOW);
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
  WiFi.softAP(wifiSsid, wifiPassword);
  delay(100); // allow AP to start
  Serial.println("WiFi AP started");
  Serial.println("SSID: " + String(wifiSsid));
  Serial.println("Password: " + String(wifiPassword));
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

    int estop = 0, up = 0, down = 0, left = 0, right = 0;
    int parsed = sscanf(str, "%d,%d,%d,%d,%d", &estop, &up, &down, &left, &right);

    if (parsed < 3) {
      Serial.printf("Parse error: only %d values found in: %s\n", parsed, value.c_str());
      return;
    }
    if (parsed < 4) left = 0;
    if (parsed < 5) right = 0;

    Serial.printf("Received: [%d,%d,%d,%d,%d]\n", estop, up, down, left, right);

    // Safety: UP/DOWN and LEFT/RIGHT pairs must never both be active at the same time
    if (up && down) {
      up   = 0;
      down = 0;
      Serial.println("CONFLICT: UP+DOWN both active — both forced OFF");
    }
    if (left && right) {
      left  = 0;
      right = 0;
      Serial.println("CONFLICT: LEFT+RIGHT both active — both forced OFF");
    }

    if (estop == 1) {
      // Emergency stop — all outputs OFF (including E-STOP output)
      motionEstop = true;
      motionUp    = false;
      motionDown  = false;
      motionLeft  = false;
      motionRight = false;
      Serial.println("E-STOP ACTIVE: E-STOP/Others OFF");
    } else {
      // Normal operation
      motionEstop = false;
      motionUp    = (up    != 0);
      motionDown  = (down  != 0);
      motionLeft  = (left  != 0);
      motionRight = (right != 0);
      Serial.printf("E-STOP:ON UP:%d DOWN:%d LEFT:%d RIGHT:%d\n", up, down, left, right);
    }

    updateOutputPinsFromState();
    notifyMotionStatus();
  }
};

class HeartbeatCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    if (!authenticated) {
      Serial.println("Heartbeat ignored — not authenticated");
      return;
    }

    std::string data = pCharacteristic->getValue();
    if (data != "HB") {
      Serial.printf("Invalid heartbeat payload: '%s'\n", data.c_str());
      return;
    }

    lastHeartbeatTime = millis();
    heartbeatMissCount = 0;
    if (!heartbeatAlive) {
      heartbeatAlive = true;
      Serial.println("Heartbeat restored — outputs enabled if command state allows");
    }
    updateOutputPinsFromState();
  }
};

void setup() {
  Serial.begin(115200);

  loadUsers();
  loadOutputConfig();  // reads NVS, applies pinMode + LOW for all 5 outputs
  // Explicitly configure digital outputs and 24V supply pins
  pinMode(Q0_0, OUTPUT);
  pinMode(Q0_1, OUTPUT);
  pinMode(Q0_2, OUTPUT);
  pinMode(Q0_3, OUTPUT);

 pinMode(S0_24v, OUTPUT);
 pinMode(S1_24v, OUTPUT);
 pinMode(S2_24v, OUTPUT);
 pinMode(S3_24v, OUTPUT);

 digitalWrite(S0_24v, HIGH);
 digitalWrite(S1_24v, HIGH);
 digitalWrite(S2_24v, HIGH);
 digitalWrite(S3_24v, HIGH);

  // Ensure all configured output pins are set to OUTPUT on startup
  for (int i = 0; i < NUM_OUTPUT_PINS; i++) {
    digitalWrite(OUTPUT_PINS[i].pin, LOW);
  }
  loadNetworkConfig();
  forceAllOutputsOff();

  startWiFi();

  setupWebRoutes();

  BLEDevice::init(bleName);

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

  heartbeatChar = pService->createCharacteristic(
    HEARTBEAT_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  heartbeatChar->setCallbacks(new HeartbeatCallbacks());

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

    unsigned long elapsed = lastHeartbeatTime == 0 ? ULONG_MAX : (millis() - lastHeartbeatTime);
    int newMissCount = 0;
    if (elapsed > 300) newMissCount = 3;
    else if (elapsed > 200) newMissCount = 2;
    else if (elapsed > 100) newMissCount = 1;

    if (newMissCount != heartbeatMissCount) {
      heartbeatMissCount = newMissCount;
      if (heartbeatMissCount == 1) {
        Serial.println("Heartbeat not received within 100ms");
      } else if (heartbeatMissCount == 2) {
        Serial.println("Heartbeat not received within 200ms");
      } else if (heartbeatMissCount >= 3) {
        if (heartbeatAlive) {
          heartbeatAlive = false;
          Serial.println("Heartbeat missing for 300ms — outputs turned OFF");
        }
      }
    }

    updateOutputPinsFromState();
  } else {
    heartbeatAlive = false;
    forceAllOutputsOff();
  }

  handleWiFi(); // does nothing in AP mode
  webServer.handleClient();

  delay(20);
}