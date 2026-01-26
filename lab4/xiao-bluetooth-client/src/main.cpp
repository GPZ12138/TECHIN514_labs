#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <cctype>
#include <cstdlib>
#include <cmath>

// ===== UUIDs (match server) =====
static BLEUUID serviceUUID("36a02f47-375d-43aa-8d99-94fdcad10669");
static BLEUUID charUUID("dc2fef20-3b41-4fff-98a0-d4fffa143262");

// ===== BLE state =====
static bool doConnect = false;
static bool connected = false;
static bool doScan = false;

static BLERemoteCharacteristic* pRemoteCharacteristic = nullptr;
static BLEAdvertisedDevice* myDevice = nullptr;

// ===== Data stats since connection =====
static float g_currentDistance = NAN;
static float g_minDistance = NAN;
static float g_maxDistance = NAN;

// ===== Extra field reception: device name string =====
static String g_deviceName = "";
static bool g_deviceNameReceived = false;

static void resetStats() {
  g_currentDistance = NAN;
  g_minDistance = NAN;
  g_maxDistance = NAN;
  g_deviceName = "";
  g_deviceNameReceived = false;
}

static void updateStats(float v) {
  g_currentDistance = v;
  if (isnan(g_minDistance) || v < g_minDistance) g_minDistance = v;
  if (isnan(g_maxDistance) || v > g_maxDistance) g_maxDistance = v;
}

// Parse number from payload like "12.34 cm" or "Distance: 12.34"
static bool parseDistance(const uint8_t* data, size_t len, float& outValue) {
  if (!data || len == 0) return false;

  char buf[128];
  size_t n = (len < sizeof(buf) - 1) ? len : (sizeof(buf) - 1);
  memcpy(buf, data, n);
  buf[n] = '\0';

  char* p = buf;

  // skip until a number-like char
  while (*p) {
    if (isdigit((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.') break;
    p++;
  }
  if (!*p) return false;

  char* endPtr = nullptr;
  double v = strtod(p, &endPtr);
  if (endPtr == p) return false;

  outValue = (float)v;
  return true;
}

// Store non-numeric payload as a string field (e.g., "Kitty's MCU")
static void handleNonNumericPayload(const uint8_t* data, size_t len) {
  if (!data || len == 0) return;

  // Convert to String safely
  String s;
  s.reserve(len);
  for (size_t i = 0; i < len; i++) {
    s += (char)data[i];
  }

  s.trim();
  if (s.length() == 0) return;

  // Save as device name only once, print once
  if (!g_deviceNameReceived) {
    g_deviceName = s;
    g_deviceNameReceived = true;

    Serial.print("Device name received: ");
    Serial.println(g_deviceName);
  }
}

static void notifyCallback(
  BLERemoteCharacteristic* pBLERemoteCharacteristic,
  uint8_t* pData,
  size_t length,
  bool isNotify
) {
  float v = NAN;

  if (!parseDistance(pData, length, v)) {
    handleNonNumericPayload(pData, length);
    return;
  }

  updateStats(v);

  // Print current, min, max every time numeric data arrives
  if (g_deviceNameReceived) {
    Serial.print("From: ");
    Serial.print(g_deviceName);
    Serial.print(" | ");
  }

  Serial.print("Distance: ");
  Serial.print(g_currentDistance, 2);
  Serial.print(" cm | Min: ");
  Serial.print(g_minDistance, 2);
  Serial.print(" cm | Max: ");
  Serial.print(g_maxDistance, 2);
  Serial.println(" cm");
}

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) override {
    resetStats();
  }

  void onDisconnect(BLEClient* pclient) override {
    connected = false;
    Serial.println("onDisconnect");
  }
};

bool connectToServer() {
  Serial.print("Forming a connection to ");
  Serial.println(myDevice->getAddress().toString().c_str());

  BLEClient* pClient = BLEDevice::createClient();
  Serial.println(" - Created client");

  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(myDevice)) {
    Serial.println(" - Failed to connect");
    return false;
  }
  Serial.println(" - Connected to server");

  pClient->setMTU(517);

  BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
  if (pRemoteService == nullptr) {
    Serial.print("Failed to find our service UUID: ");
    Serial.println(serviceUUID.toString().c_str());
    pClient->disconnect();
    return false;
  }
  Serial.println(" - Found our service");

  pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUID);
  if (pRemoteCharacteristic == nullptr) {
    Serial.print("Failed to find our characteristic UUID: ");
    Serial.println(charUUID.toString().c_str());
    pClient->disconnect();
    return false;
  }
  Serial.println(" - Found our characteristic");

  // Optional initial read, also feed into the same handlers
  if (pRemoteCharacteristic->canRead()) {
    std::string value = pRemoteCharacteristic->readValue();
    Serial.print("Initial characteristic value: ");
    Serial.println(value.c_str());

    // If initial value contains device name, record it
    float tmp = NAN;
    if (!parseDistance((const uint8_t*)value.data(), value.size(), tmp)) {
      handleNonNumericPayload((const uint8_t*)value.data(), value.size());
    }
  }

  if (pRemoteCharacteristic->canNotify()) {
    pRemoteCharacteristic->registerForNotify(notifyCallback);
    Serial.println(" - Registered for notify");
  } else {
    Serial.println(" - Characteristic cannot notify");
  }

  connected = true;
  return true;
}

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    Serial.print("BLE Advertised Device found: ");
    Serial.println(advertisedDevice.toString().c_str());

    if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(serviceUUID)) {
      BLEDevice::getScan()->stop();
      myDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
      doScan = true;
    }
  }
};

void setup() {
  Serial.begin(115200);
  Serial.println("Starting Arduino BLE Client application...");
  BLEDevice::init("");

  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setInterval(1349);
  pBLEScan->setWindow(449);
  pBLEScan->setActiveScan(true);

  pBLEScan->start(5, false);
}

void loop() {
  if (doConnect) {
    if (connectToServer()) {
      Serial.println("We are now connected to the BLE Server.");
    } else {
      Serial.println("We have failed to connect to the server.");
    }
    doConnect = false;
  }

  // Client side only receives notifications for this lab.

  if (!connected && doScan) {
    BLEDevice::getScan()->start(0);
  }

  delay(50);
}
