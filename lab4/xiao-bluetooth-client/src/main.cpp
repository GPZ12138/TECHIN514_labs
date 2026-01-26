#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <cctype>
#include <cstdlib>

// ===== Correct UUIDs you provided =====
static BLEUUID serviceUUID("36a02f47-375d-43aa-8d99-94fdcad10669");
static BLEUUID charUUID("dc2fef20-3b41-4fff-98a0-d4fffa143262");

// ===== BLE state =====
static boolean doConnect = false;
static boolean connected = false;
static boolean doScan = false;

static BLERemoteCharacteristic* pRemoteCharacteristic = nullptr;
static BLEAdvertisedDevice* myDevice = nullptr;

// ===== Data collection globals (TODO done) =====
static float g_currentDistance = NAN;
static float g_minDistance = NAN;
static float g_maxDistance = NAN;

// Reset stats for each new connection
static void resetStats() {
  g_currentDistance = NAN;
  g_minDistance = NAN;
  g_maxDistance = NAN;
}

// Update min/max with a new value
static void updateStats(float v) {
  g_currentDistance = v;

  if (isnan(g_minDistance) || v < g_minDistance) g_minDistance = v;
  if (isnan(g_maxDistance) || v > g_maxDistance) g_maxDistance = v;
}

// Try to parse distance number from payload (string or raw bytes)
static bool parseDistance(const uint8_t* data, size_t len, float& outValue) {
  if (data == nullptr || len == 0) return false;

  // Copy to a null-terminated buffer so we can treat it as a string safely
  static char buf[128];
  size_t n = (len < sizeof(buf) - 1) ? len : (sizeof(buf) - 1);
  memcpy(buf, data, n);
  buf[n] = '\0';

  // Trim leading spaces
  char* p = buf;
  while (*p && isspace(static_cast<unsigned char>(*p))) p++;

  // Accept formats like: "12.34", "12", "Distance: 12.34", "cm=12.34"
  // Find first char that can start a number: digit, +, -, .
  while (*p) {
    if (isdigit(static_cast<unsigned char>(*p)) || *p == '+' || *p == '-' || *p == '.') break;
    p++;
  }
  if (!*p) return false;

  char* endPtr = nullptr;
  double v = strtod(p, &endPtr);
  if (endPtr == p) return false;

  outValue = static_cast<float>(v);
  return true;
}

// ===== Notify callback (TODO done) =====
static void notifyCallback(
  BLERemoteCharacteristic* pBLERemoteCharacteristic,
  uint8_t* pData,
  size_t length,
  bool isNotify
) {
  float v = NAN;
  if (!parseDistance(pData, length, v)) {
    Serial.print("Received non-numeric data (len=");
    Serial.print(length);
    Serial.print("): ");
    Serial.write(pData, length);
    Serial.println();
    return;
  }

  // Update aggregation
  updateStats(v);

  // Print current, max, min every time new data arrives
  Serial.print("Distance: ");
  Serial.print(g_currentDistance, 3);
  Serial.print(" | Min: ");
  Serial.print(g_minDistance, 3);
  Serial.print(" | Max: ");
  Serial.print(g_maxDistance, 3);
  Serial.println();
}

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) override {
    resetStats();  // Start fresh stats per connection
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

  // Connect to the remote BLE Server.
  if (!pClient->connect(myDevice)) {
    Serial.println(" - Failed to connect");
    return false;
  }
  Serial.println(" - Connected to server");

  // Request maximum MTU from server (optional)
  pClient->setMTU(517);

  // Obtain a reference to the service we are after in the remote BLE server.
  BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
  if (pRemoteService == nullptr) {
    Serial.print("Failed to find our service UUID: ");
    Serial.println(serviceUUID.toString().c_str());
    pClient->disconnect();
    return false;
  }
  Serial.println(" - Found our service");

  // Obtain a reference to the characteristic in the service of the remote BLE server.
  pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUID);
  if (pRemoteCharacteristic == nullptr) {
    Serial.print("Failed to find our characteristic UUID: ");
    Serial.println(charUUID.toString().c_str());
    pClient->disconnect();
    return false;
  }
  Serial.println(" - Found our characteristic");

  // Optional: read once at connect
  if (pRemoteCharacteristic->canRead()) {
    std::string value = pRemoteCharacteristic->readValue();
    Serial.print("Initial characteristic value: ");
    Serial.println(value.c_str());
  }

  // Register for notifications
  if (pRemoteCharacteristic->canNotify()) {
    pRemoteCharacteristic->registerForNotify(notifyCallback);
    Serial.println(" - Registered for notify");
  } else {
    Serial.println(" - Characteristic cannot notify");
  }

  connected = true;
  return true;
}

/**
 * Scan for BLE servers and find the first one that advertises the service we are looking for.
 */
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
  if (doConnect == true) {
    if (connectToServer()) {
      Serial.println("We are now connected to the BLE Server.");
    } else {
      Serial.println("We have failed to connect to the server; nothing more we will do.");
    }
    doConnect = false;
  }

  // Client assignment focuses on receiving notifications.
  // Keeping loop lightweight reduces interference with server data flow.

  if (!connected && doScan) {
    BLEDevice::getScan()->start(0);
  }

  delay(50);
}
