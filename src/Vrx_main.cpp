#include <Arduino.h>

#if defined(PLATFORM_ESP8266)
#include <espnow.h>
#include <ESP8266WiFi.h>
#elif defined(PLATFORM_ESP32)
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#endif

#include <FastLED.h>

#include "msp.h"
#include "msptypes.h"
#include "logging.h"
#include "helpers.h"
#include "common.h"
#include "options.h"
#include "config.h"
#include "crsf_protocol.h"

#include "device.h"
#include "devWIFI.h"
#include "devButton.h"
#include "devLED.h"

#ifdef RAPIDFIRE_BACKPACK
#include "rapidfire.h"
#elif defined(RX5808_BACKPACK)
#include "rx5808.h"
#elif defined(STEADYVIEW_BACKPACK)
#include "steadyview.h"
#elif defined(FUSION_BACKPACK)
#include "fusion.h"
#elif defined(HDZERO_BACKPACK)
#include "hdzero.h"
#elif defined(SKYZONE_MSP_BACKPACK)
#include "skyzone_msp.h"
#elif defined(ORQA_BACKPACK)
#include "orqa.h"
#endif

#include <Wire.h>
#include <VL53L0X.h>

/////////// DEFINES ///////////

#define BINDING_TIMEOUT 5000  // 5 seconds
#define BINDING_BOOT_COUNT 10 // Reboot into binding mode after 10 sub-5-second boots
#define NO_BINDING_TIMEOUT 120000
#define BINDING_LED_PAUSE 1000

#if !defined(VRX_BOOT_DELAY)
#define VRX_BOOT_DELAY 0
#endif

#if !defined(VRX_UART_BAUD)
#define VRX_UART_BAUD 460800
#endif

// LED defines
#define NUM_LEDS 32
#define LED_PIN 12
#define LED_UPDATE_INTERVAL 16

#define SPEED 3
#define PATTERN_WIDTH 20

// Read the VL53L0X every 200ms
#define SENSOR_UPDATE_INTERVAL 200
#define LED_ACTIVE_DISTANCE 50 // mm

/////////// GLOBALS ///////////

uint8_t backpackVersion[] = {LATEST_VERSION, 0};

connectionState_e connectionState = starting;
unsigned long bindingStart = 0;
unsigned long rebootTime = 0;

uint8_t cachedIndex = 0;
bool sendChannelChangesToVrx = false;
bool sendHeadTrackingChangesToVrx = false;
bool sendRTCChangesToVrx = false;
bool gotInitialPacket = false;
bool headTrackingEnabled = false;
uint32_t lastSentRequest = 0;

device_t *ui_devices[] = {
#ifdef PIN_LED
    &LED_device,
#endif
#ifdef PIN_BUTTON
    &Button_device,
#endif
    &WIFI_device,
};

#if defined(PLATFORM_ESP32)
// This seems to need to be global, as per this page,
// otherwise we get errors about invalid peer:
// https://rntlab.com/question/espnow-peer-interface-is-invalid/
esp_now_peer_info_t peerInfo;
#endif

uint8_t theta = 0;
uint8_t sat = 255;
uint8_t speed = SPEED;

// The hue increment for each LED
uint8_t patternWidth = 20;

// The last time the LEDs were updated
uint32_t lastLEDUpdate = 0;
bool vrxRecording = false;

// Set up the sensor
VL53L0X sensor;
uint32_t sensorLastUpdate = 0;
bool ledsActive = false;

/////////// CLASS OBJECTS ///////////

MSP msp;
CRGB leds[NUM_LEDS];

ELRS_EEPROM eeprom;
VrxBackpackConfig config;

#ifdef RAPIDFIRE_BACKPACK
Rapidfire vrxModule;
#elif defined(RX5808_BACKPACK)
RX5808 vrxModule;
#elif defined(STEADYVIEW_BACKPACK)
SteadyView vrxModule;
#elif defined(FUSION_BACKPACK)
Fusion vrxModule;
#elif defined(HDZERO_BACKPACK)
HDZero vrxModule(&Serial);
#elif defined(SKYZONE_MSP_BACKPACK)
SkyzoneMSP vrxModule(&Serial);
#elif defined(ORQA_BACKPACK)
Orqa vrxModule;
#endif

/////////// FUNCTION DEFS ///////////

void ProcessMSPPacket(mspPacket_t *packet);
void sendMSPViaEspnow(mspPacket_t *packet);
void resetBootCounter();
void SetupEspNow();

/////////////////////////////////////

void RebootIntoWifi()
{
  DBGLN("Rebooting into wifi update mode...");
  config.SetStartWiFiOnBoot(true);
  config.SetBootCount(0);
  config.Commit();
  rebootTime = millis();
}

// espnow on-receive callback
#if defined(PLATFORM_ESP8266)
void OnDataRecv(uint8_t *mac_addr, uint8_t *data, uint8_t data_len)
#elif defined(PLATFORM_ESP32)
void OnDataRecv(const uint8_t *mac_addr, const uint8_t *data, int data_len)
#endif
{
  DBGLN("ESP NOW DATA:");
  for (int i = 0; i < data_len; i++)
  {
    DBG("%x", data[i]); // Debug prints
    DBG(",");

    if (msp.processReceivedByte(data[i]))
    {
      DBGLN(""); // Extra line for serial output readability
      // Finished processing a complete packet
      // Only process packets from a bound MAC address
      if (connectionState == binding ||
          (firmwareOptions.uid[0] == mac_addr[0] &&
           firmwareOptions.uid[1] == mac_addr[1] &&
           firmwareOptions.uid[2] == mac_addr[2] &&
           firmwareOptions.uid[3] == mac_addr[3] &&
           firmwareOptions.uid[4] == mac_addr[4] &&
           firmwareOptions.uid[5] == mac_addr[5]))
      {
        gotInitialPacket = true;
        ProcessMSPPacket(msp.getReceivedPacket());
      }
      else
      {
        DBGLN("Failed MAC add check and not in bindingMode.");
      }
      msp.markPacketReceived();
    }
  }
  blinkLED();
}

void ProcessMSPPacket(mspPacket_t *packet)
{
  if (connectionState == binding)
  {
    DBGLN("Processing Binding Packet...");
    if (packet->function == MSP_ELRS_BIND)
    {
      config.SetGroupAddress(packet->payload);
      DBGLN("MSP_ELRS_BIND MAC = ");
      for (int i = 0; i < 6; i++)
      {
        DBG("%x", packet->payload[i]); // Debug prints
        DBG(",");
      }
      DBG(""); // Extra line for serial output readability
      resetBootCounter();
      connectionState = running;
      rebootTime = millis() + 200; // Add 200ms to allow for any response message(s) to be sent back to device
    }
    return;
  }

  switch (packet->function)
  {
  case MSP_SET_VTX_CONFIG:
    DBGLN("Processing MSP_SET_VTX_CONFIG...");
    if (packet->payload[0] < 48) // Standard 48 channel VTx table size e.g. A, B, E, F, R, L
    {
      // cache changes here, to be handled outside this callback, in the main loop
      cachedIndex = packet->payload[0];
      ;
      sendChannelChangesToVrx = true;
    }
    else
    {
      return; // Packets containing frequency in MHz are not yet supported.
    }
    break;
  case MSP_ELRS_SET_VRX_BACKPACK_WIFI_MODE:
    DBGLN("Processing MSP_ELRS_SET_VRX_BACKPACK_WIFI_MODE...");
    RebootIntoWifi();
    break;
  case MSP_ELRS_BACKPACK_SET_RECORDING_STATE:
    DBGLN("Processing MSP_ELRS_BACKPACK_SET_RECORDING_STATE...");
    {
      uint8_t state = packet->readByte();
      uint8_t lowByte = packet->readByte();
      uint8_t highByte = packet->readByte();
      uint16_t delay = lowByte | highByte << 8;
      vrxModule.SetRecordingState(state, delay);

      // Set the recordingState global
      vrxRecording = (bool)state;
    }
    break;
  case MSP_ELRS_SET_OSD:
    vrxModule.SetOSD(packet);
    break;
  case MSP_ELRS_BACKPACK_SET_HEAD_TRACKING:
    DBGLN("Processing MSP_ELRS_BACKPACK_SET_HEAD_TRACKING...");
    headTrackingEnabled = packet->readByte();
    sendHeadTrackingChangesToVrx = true;
    break;
  case MSP_ELRS_BACKPACK_CRSF_TLM:
    DBGV("Processing MSP_ELRS_BACKPACK_CRSF_TLM type %x\n", packet->payload[1]);
    if (packet->payloadSize < 4)
    {
      DBGLN("CRSF_TLM packet too short")
      break;
    }
    switch (packet->payload[2])
    {
    case CRSF_FRAMETYPE_BATTERY_SENSOR:
      vrxModule.SendBatteryTelemetry(packet->payload);
      break;
    case CRSF_FRAMETYPE_LINK_STATISTICS:
      vrxModule.SendLinkTelemetry(packet->payload);
      break;
    }
    break;
  default:
    DBGLN("Unknown command from ESPNOW");
    break;
  }
}

void SetupEspNow()
{
  if (esp_now_init() != 0)
  {
    DBGLN("Error initializing ESP-NOW");
    turnOffLED();
    ESP.restart();
  }

#if defined(PLATFORM_ESP8266)
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_add_peer(firmwareOptions.uid, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
#elif defined(PLATFORM_ESP32)
  memcpy(peerInfo.peer_addr, firmwareOptions.uid, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) != ESP_OK)
  {
    DBGLN("ESP-NOW failed to add peer");
    return;
  }
#endif

  esp_now_register_recv_cb(OnDataRecv);
}

void SetSoftMACAddress()
{
  DBGLN("EEPROM MAC = ");
  for (int i = 0; i < 6; i++)
  {
#ifndef MY_UID
    memcpy(firmwareOptions.uid, config.GetGroupAddress(), 6);
#endif
    DBG("%x", firmwareOptions.uid[i]); // Debug prints
    DBG(",");
  }
  DBGLN(""); // Extra line for serial output readability

  // MAC address can only be set with unicast, so first byte must be even, not odd
  firmwareOptions.uid[0] = firmwareOptions.uid[0] & ~0x01;

  WiFi.mode(WIFI_STA);
  WiFi.begin("network-name", "pass-to-network", 1);
  WiFi.disconnect();

// Soft-set the MAC address to the passphrase UID for binding
#if defined(PLATFORM_ESP8266)
  wifi_set_macaddr(STATION_IF, firmwareOptions.uid);
#elif defined(PLATFORM_ESP32)
  esp_wifi_set_mac(WIFI_IF_STA, firmwareOptions.uid);
#endif
}

void RequestVTXPacket()
{
  mspPacket_t packet;
  packet.reset();
  packet.makeCommand();
  packet.function = MSP_ELRS_REQU_VTX_PKT;
  packet.addByte(0); // empty byte

  blinkLED();
  sendMSPViaEspnow(&packet);
}

void sendMSPViaEspnow(mspPacket_t *packet)
{
  // Do not send while in binding mode.  The currently used firmwareOptions.uid may be garbage.
  if (connectionState == binding)
    return;

  uint8_t packetSize = msp.getTotalPacketSize(packet);
  uint8_t nowDataOutput[packetSize];

  uint8_t result = msp.convertToByteArray(packet, nowDataOutput);

  if (!result)
  {
    // packet could not be converted to array, bail out
    return;
  }

  esp_now_send(firmwareOptions.uid, (uint8_t *)&nowDataOutput, packetSize);
}

void resetBootCounter()
{
  config.SetBootCount(0);
  config.Commit();
}

void checkIfInBindingMode()
{
  uint8_t bootCounter = config.GetBootCount();
  bootCounter++;

  if (bootCounter >= 4) // 10 sub-5-second boots
  {
    resetBootCounter();

#ifdef MY_UID
    RebootIntoWifi();
#else
    connectionState = binding;
    bindingStart = millis();
#endif
  }
  else
  {
    config.SetBootCount(bootCounter);
    config.Commit();
  }

  DBGLN(""); // blank line to clear below prints from serial boot garbage
  DBG("bootCounter = ");
  DBGLN("%x", bootCounter);
  DBG("bindingMode = ");
  DBGLN("%x", connectionState == binding);
}

bool BindingExpired(uint32_t now)
{
  return (connectionState == binding) && ((now - bindingStart) > NO_BINDING_TIMEOUT);
}

#if defined(PLATFORM_ESP8266)
// Called from core's user_rf_pre_init() function (which is called by SDK) before setup()
RF_PRE_INIT()
{
// Set whether the chip will do RF calibration or not when power up.
// I believe the Arduino core fakes this (byte 114 of phy_init_data.bin)
// to be 1, but the TX power calibration can pull over 300mA which can
// lock up receivers built with a underspeced LDO (such as the EP2 "SDG")
// Option 2 is just VDD33 measurement
#if defined(RF_CAL_MODE)
  system_phy_set_powerup_option(RF_CAL_MODE);
#else
  system_phy_set_powerup_option(2);
#endif
}
#endif

void setup()
{
#if !defined(HDZERO_BACKPACK)
  // Serial.begin() seems to prevent the HDZ VRX from booting
  // If we're not on HDZ, init serial early for debug msgs
  // Otherwise, delay it till the end of setup
  Serial.begin(VRX_UART_BAUD);
#endif

  options_init();

  eeprom.Begin();
  config.SetStorageProvider(&eeprom);
  config.Load();

  devicesInit(ui_devices, ARRAY_SIZE(ui_devices));

#ifdef DEBUG_ELRS_WIFI
  config.SetStartWiFiOnBoot(true);
#endif

  if (config.GetStartWiFiOnBoot())
  {
    config.SetStartWiFiOnBoot(false);
    config.Commit();
    connectionState = wifiUpdate;
    devicesTriggerEvent();
  }
  else
  {
#if !defined(NO_AUTOBIND)
    checkIfInBindingMode();
#endif
    SetSoftMACAddress();
    SetupEspNow();
  }

  devicesStart();
  if (connectionState == starting)
  {
    connectionState = running;
  }

  // Initialise LEDs
  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  // .setCorrection(TypicalLEDStrip);
  FastLED.clear();
  FastLED.show();

  vrxModule.Init();
#if defined(HDZERO_BACKPACK)
  Serial.begin(VRX_UART_BAUD);
#endif

  // Initialise the VL53L0X
  Wire.begin(4, 5);

  sensor.setTimeout(500);
  if (!sensor.init())
  {
    DBGLN("Failed to detect and initialize VL53L0X!");
  }
  // Start continuous back-to-back mode every 200ms
  sensor.startContinuous(200);

  DBGLN("Setup completed");
}

void loop()
{
  uint32_t now = millis();

  devicesUpdate(now);
  vrxModule.Loop(now);

  // Update the sensor
  if (now - sensorLastUpdate > SENSOR_UPDATE_INTERVAL)
  {
    sensorLastUpdate = now;
    uint16_t range = sensor.readRangeContinuousMillimeters();
    if (sensor.timeoutOccurred())
    {
      DBGLN("Sensor timeout");
    }
    else
    {
      // Activate the LEDs if the distance is less than LED_ACTIVE_DISTANCE
      ledsActive = range < LED_ACTIVE_DISTANCE;
      DBGLN("Distance: %d mm", range)
    }
  }

  // Update the LEDs
  if (now - lastLEDUpdate > LED_UPDATE_INTERVAL)
  {
    if (vrxRecording)
    {
      // If we're recording, flash the LEDs red
      speed = 6;
      for (int i = 0; i < NUM_LEDS; i++)
        leds[i].setHSV(0, sat, sin8(theta * 1.5));
    }
    else
    {
      speed = SPEED;

      for (int i = 0; i < NUM_LEDS / 2; i++)
      {
        // Set the hue and value of the LEDs
        uint8_t ledTheta = i * PATTERN_WIDTH;
        uint8_t hueChoice = squarewave8((theta + ledTheta));
        uint8_t ledHue = hueChoice ? HUE_YELLOW : HUE_BLUE;

        ledTheta *= 2;
        uint8_t ledVal = cubicwave8((theta * 2 + ledTheta));
        // ledVal = 255;
        leds[i].setHSV(ledHue, sat, dim8_lin(ledVal));

        // Mirror the LEDs to the other side
        for (int i = 0; i < NUM_LEDS / 2; i++)
          leds[NUM_LEDS - i - 1] = leds[i];
      }
    }

    // Only show the LEDs if they're active
    if (ledsActive)
      FastLED.show();
    else
      FastLED.clear(true);

    theta += speed;
    lastLEDUpdate = now;
  }

#if defined(PLATFORM_ESP8266) || defined(PLATFORM_ESP32)
  // If the reboot time is set and the current time is past the reboot time then reboot.
  if (rebootTime != 0 && now > rebootTime)
  {
    turnOffLED();
    ESP.restart();
  }
#endif

  if (connectionState == wifiUpdate)
  {
    if (sendRTCChangesToVrx)
    {
      sendRTCChangesToVrx = false;
      vrxModule.SetRTC();
    }
    return;
  }

  if (BindingExpired(now))
  {
    DBGLN("Binding expired");
#if !defined(NO_AUTOBIND)
    RebootIntoWifi();
#else
    connectionState = running;
#endif
  }

  if (sendChannelChangesToVrx)
  {
    sendChannelChangesToVrx = false;
    vrxModule.SendIndexCmd(cachedIndex);
  }
  if (sendHeadTrackingChangesToVrx)
  {
    sendHeadTrackingChangesToVrx = false;
    vrxModule.SendHeadTrackingEnableCmd(headTrackingEnabled);
  }

  // spam out a bunch of requests for the desired band/channel for the first 5s
  if (!gotInitialPacket && now - VRX_BOOT_DELAY < 5000 && now - lastSentRequest > 1000 && connectionState != binding)
  {
    DBGLN("RequestVTXPacket...");
    RequestVTXPacket();
    lastSentRequest = now;
  }

#if !defined(NO_AUTOBIND)
  // Power cycle must be done within 5s.  Long timeout to allow goggles to boot and shutdown correctly e.g. Orqa.
  if (now > BINDING_TIMEOUT && config.GetBootCount() > 0)
  {
    DBGLN("resetBootCounter...");
    resetBootCounter();
  }
#endif
}
