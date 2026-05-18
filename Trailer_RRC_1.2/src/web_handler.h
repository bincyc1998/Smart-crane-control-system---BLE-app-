#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include <Preferences.h>

// -------- Web admin credentials (mutable, stored in NVS namespace "webadmin") --------
// Defaults used on first boot; changed via the Account tab in the web UI.

// -------- BLE user store --------
#define MAX_USERS 10

struct User {
  char name[64];
  char email[64];
  char password[64];
  char role[32];
};

// Shared globals defined in main.cpp
extern User        users[MAX_USERS];
extern int         userCount;
extern WebServer   webServer;
extern Preferences prefs;



// BLE connection state — defined in main.cpp
extern bool deviceConnected;
extern bool authenticated;
extern char connectedUserEmail[64];

// Current motion output state — defined in main.cpp, updated by BLE callback
extern bool motionEstop;
extern bool motionUp;
extern bool motionDown;

// Output pin configuration — defined in main.cpp
#define NUM_OUTPUT_PINS 5
struct OutPin {
  const char* label;
  int         pin;
};
extern const OutPin OUTPUT_PINS[NUM_OUTPUT_PINS];
extern uint8_t outIdxEstop;
extern uint8_t outIdxUp;
extern uint8_t outIdxDown;

// BLE / WiFi credentials — defined in main.cpp
extern char bleName[32];
extern char wifiSsid[32];
extern char wifiPassword[64];

// NVS helpers defined in main.cpp, called from web handlers
void saveUsers();
void saveOutputConfig();
void applyOutputConfig();
void saveNetworkConfig();
void loadNetworkConfig();

// Web route registration — call once in setup()
void setupWebRoutes();
