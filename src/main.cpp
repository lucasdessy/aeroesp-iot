#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// Bluetooth stuff
#include "BluetoothSerial.h"

char wifiSsid[64];
char wifiPass[64];
char mqttServer[128];
int mqttServerPort = 0;

const int idleTimeMs = 10000;
const int timeToReset = 2000;
const char *storageName = "credentials";
const char *hostName = "aeroEsp";
const char *mqttSubTopicName = "aeroEsp/in";
const char *mqttPublishTopicName = "aeroEsp/out";

#define LED 2
#define BUTTON 0

// Bluetooth stuff
BluetoothSerial SerialBT;

Preferences storage;
TaskHandle_t ledTask;

WiFiClient espClient;
PubSubClient mqtt(espClient);

long lastPubMsg = 0;

enum LedState
{
  ledBlinkNetActivity,
  ledBlinkServerConnection,
  ledBlinkWifiConnection,
  ledBlinkBluetoothConnection,
  ledStill,
  ledOff
};

LedState currentLedState = ledOff;
bool deviceConnected = false;

void setupBluetooth()
{
  currentLedState = ledBlinkBluetoothConnection;
  SerialBT.begin("AeroESP");
  bool gotAllInfo = false;
  String messageString;
  while (!gotAllInfo)
  {
    if (!SerialBT.available())
      continue;

    messageString = String();
    while (SerialBT.available())
    {
      messageString += (char)SerialBT.read();
    }

    StaticJsonDocument<64> jsonResponse;
    char jsonResponseChar[64];
    jsonResponse["ok"] = false;

    StaticJsonDocument<256> json;
    DeserializationError err = deserializeJson(json, messageString);
    Serial.print("Bluetooth message received: ");
    Serial.println(messageString);
    if (err)
    {
      Serial.print(F("deserializeJson() failed with code "));
      Serial.println(err.f_str());
    }
    else
    {
      String ssid = json["ssid"];
      String password = json["password"];
      String server = json["server"];
      int serverPort = json["serverPort"];
      if (!ssid.isEmpty() && !password.isEmpty() && !server.isEmpty() && (serverPort != 0))
      {
        ssid.toCharArray(wifiSsid, 64);
        password.toCharArray(wifiPass, 64);
        server.toCharArray(mqttServer, 128);
        mqttServerPort = serverPort;

        Serial.println("Saving info on memory...");
        storage.begin(storageName, false);
        storage.putString("ssid", ssid);
        storage.putString("password", password);
        storage.putString("server", server);
        storage.putInt("serverPort", serverPort);
        storage.end();
        Serial.println("All info saved successfully");

        jsonResponse["ok"] = true;
        gotAllInfo = true;
      }
      else
      {
        char jsonChar[256];
        serializeJson(json, jsonChar);
        Serial.print("Got json, didn't get all info, json:");
        Serial.println(jsonChar);
      }
      serializeJson(jsonResponse, jsonResponseChar);
      SerialBT.println(jsonResponseChar);
      SerialBT.flush();
    }
  }
  ESP.restart();
}

void setupCredentials()
{
  currentLedState = ledBlinkBluetoothConnection;
  storage.begin(storageName, false);
  String ssid = storage.getString("ssid");
  String password = storage.getString("password");
  String server = storage.getString("server");
  int serverPort = storage.getInt("serverPort");
  storage.end();
  if (ssid.isEmpty())
  {
    Serial.println("ssid not found in memory... attempting to get from bluetooth.");
    return setupBluetooth();
  }
  if (password.isEmpty())
  {
    Serial.println("password not found in memory... attempting to get from bluetooth.");
    return setupBluetooth();
  }
  if (server.isEmpty())
  {
    Serial.println("server address not found in memory... attempting to get from bluetooth.");
    return setupBluetooth();
  }
  if (serverPort == 0)
  {
    Serial.println("server port not found in memory... attempting to get from bluetooth.");
    return setupBluetooth();
  }
  ssid.toCharArray(wifiSsid, 64);
  password.toCharArray(wifiPass, 64);
  server.toCharArray(mqttServer, 128);
  mqttServerPort = serverPort;
}

void mqttCallback(char *topic, byte *message, unsigned int length)
{
  Serial.print("Message arrived on topic: ");
  Serial.print(topic);
  Serial.print(". Message: ");
  String messageString;

  for (int i = 0; i < length; i++)
  {
    messageString += (char)message[i];
  }

  Serial.print(messageString);
  Serial.println();

  if (String(topic) == String(mqttSubTopicName))
  {
    DynamicJsonDocument json(128);
    DeserializationError err = deserializeJson(json, messageString);
    if (err)
    {
      Serial.print(F("deserializeJson() failed with code "));
      Serial.println(err.f_str());
      return;
    }
  }
}

void setupPubSub()
{
  mqtt.setServer(mqttServer, mqttServerPort);
  mqtt.setCallback(mqttCallback);
}

void setupWifi()
{
  currentLedState = ledBlinkWifiConnection;
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid, wifiPass);
  Serial.print("Connecting to WiFi ..");
  while (WiFi.status() != WL_CONNECTED)
  {
    Serial.print('.');
    delay(500);
  }
  Serial.println(WiFi.localIP());
}

void checkWifi()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    setupWifi();
  };
}

void checkSubMqtt()
{
  // Loop until we're reconnected
  while (!mqtt.connected())
  {
    currentLedState = ledBlinkServerConnection;
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (mqtt.connect(hostName))
    {
      Serial.println("connected");
      // Subscribe
      mqtt.subscribe(mqttSubTopicName);
      Serial.print("Subbed to: ");
      Serial.println(mqttSubTopicName);
    }
    else
    {
      Serial.print("failed, rc=");
      Serial.print(mqtt.state());
      Serial.printf(" trying again in %d seconds\n", idleTimeMs / 1000);
      delay(idleTimeMs);
    }
  }
}

float getECReadings()
{
  return random(0, 10);
}
float getPHReadings()
{
  return random(0, 14);
}

void checkPubMqtt()
{
  long now = millis();
  if (now - lastPubMsg < idleTimeMs)
  {
    return;
  }
  currentLedState = ledBlinkNetActivity;
  lastPubMsg = now;
  StaticJsonDocument<64> json;
  char jsonChar[64];

  json["ec"] = getECReadings();
  json["ph"] = getPHReadings();

  serializeJson(json, jsonChar);
  mqtt.publish(mqttPublishTopicName, jsonChar);

  Serial.print("Sent new message to topic: ");
  Serial.print(mqttPublishTopicName);
  Serial.print(". Message: ");
  Serial.println(jsonChar);
  delay(200);
}

void checkResetButton() {}

void ledBlinker(void *pvParameters)
{
  Serial.print("ledBlinker running on core ");
  Serial.println(xPortGetCoreID());
  bool ledStatus = false;
  int buttonPressedTime = 0;
  for (;;)
  {
    // ledBlinkNetActivity,
    // ledBlinkServerConnection,
    // ledBlinkWifiConnection,
    // ledStill,
    // ledOff

    while (!digitalRead(BUTTON))
    {
      delay(50);
      digitalWrite(LED, ledStatus);
      ledStatus = !ledStatus;
      buttonPressedTime++;
      if (buttonPressedTime > timeToReset / 50)
      {
        ledStatus = true;
        digitalWrite(LED, ledStatus);
        Serial.println("Reseting storage...");
        storage.begin(storageName, false);
        storage.clear();
        storage.end();
        Serial.println("Storage cleared. Restaring...");
        ESP.restart();
      }
    }
    switch (currentLedState)
    {
    case ledBlinkNetActivity:
      ledStatus = !ledStatus;
      delay(30);
      break;
    case ledBlinkServerConnection:
      ledStatus = !ledStatus;
      delay(500);
      break;
    case ledBlinkWifiConnection:
      ledStatus = !ledStatus;
      delay(1000);
      break;
    case ledBlinkBluetoothConnection:
      ledStatus = !ledStatus;
      delay(2000);
      break;
    case ledStill:
      ledStatus = true;
      delay(100);
      break;
    case ledOff:
      ledStatus = false;
      delay(100);
      break;
    }

    digitalWrite(LED, ledStatus);
  }
}

void setupPins()
{
  pinMode(LED, OUTPUT);
  pinMode(BUTTON, INPUT);
}

void setup()
{
  Serial.begin(115200);
  Serial.print("setup() running on core ");
  Serial.println(xPortGetCoreID());

  setupPins();
  xTaskCreatePinnedToCore(
      ledBlinker, /* Task function. */
      "ledTask",  /* name of task. */
      10000,      /* Stack size of task */
      NULL,       /* parameter of the task */
      1,          /* priority of the task */
      &ledTask,   /* Task handle to keep track of created task */
      0);         /* pin task to core 0 */

  setupCredentials();
  setupPubSub();
}

void loop()
{
  checkWifi();
  mqtt.loop();
  checkSubMqtt();
  checkPubMqtt();
  currentLedState = ledStill;
}
