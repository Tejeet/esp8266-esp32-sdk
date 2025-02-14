/*
 *  Copyright (c) 2019 Sinric. All rights reserved.
 *  Licensed under Creative Commons Attribution-Share Alike (CC BY-SA)
 *
 *  This file is part of the Sinric Pro (https://github.com/sinricpro/)
 */

#ifndef _SINRIC_H_
#define _SINRIC_H_

#include "SinricProInterface.h"
#include "SinricProDeviceInterface.h"
#include "SinricProWebsocket.h"
#include "SinricProUDP.h"
#include "SinricProSignature.h"
#include "SinricProMessageid.h"
#include "SinricProQueue.h"
#include "SinricProNTP.h"

class SinricProClass : public SinricProInterface {
  public:
    void begin(String socketAuthToken, String signingKey, String serverURL = SERVER_URL);
    template <typename DeviceType>
    DeviceType& add(const char* deviceId, unsigned long eventWaitTime = 1000);

    void add(SinricProDeviceInterface& newDevice);
    void add(SinricProDeviceInterface* newDevice);
    void handle();
    void stop();
    bool isConnected();

    DynamicJsonDocument prepareResponse(JsonDocument& requestMessage);
    DynamicJsonDocument prepareEvent(const char* deviceId, const char* action, const char* cause) override;
    void sendMessage(JsonDocument& jsonMessage) override;
  
    struct proxy {
      proxy(SinricProClass* ptr, String deviceId) : ptr(ptr), deviceId(deviceId) {}
      SinricProClass* ptr;
      String deviceId;
      template <typename DeviceType>
      operator DeviceType&() { return as<DeviceType>(); }
      template <typename DeviceType>
      DeviceType& as() { return ptr->getDeviceInstance<DeviceType>(deviceId); }
    };
    
    proxy operator[](const String deviceId) { return proxy(this, deviceId); }

  private:
    void handleRequest();
    void handleSendQueue();
    void connect();
    void disconnect();
    void reconnect();
    bool checkDeviceId(String deviceId);

    SinricProDeviceInterface* getDevice(String deviceId);
    
    template <typename DeviceType>
    DeviceType& getDeviceInstance(String deviceId) { return (DeviceType&) *getDevice(deviceId); }

    std::vector<SinricProDeviceInterface*> devices;
    String socketAuthToken;
    String signingKey;
    String serverURL;

    websocketListener _websocketListener;
    udpListener _udpListener;
    myNTP _ntp;
    SinricProQueue_t receiveQueue;
    SinricProQueue_t sendQueue;
};

SinricProDeviceInterface* SinricProClass::getDevice(String deviceId) {
  for (auto& device : devices) {
    if (deviceId == String(device->getDeviceId())) return device;
  }
  return nullptr;
}

void SinricProClass::begin(String socketAuthToken, String signingKey, String serverURL) {
  this->socketAuthToken = socketAuthToken;
  this->signingKey = signingKey;
  this->serverURL = serverURL;
  _ntp.begin();
}

template <typename DeviceType>
DeviceType& SinricProClass::add(const char* deviceId, unsigned long eventWaitTime) {
  DeviceType* newDevice = new DeviceType(deviceId, eventWaitTime);
  if (checkDeviceId(String(deviceId))){
    newDevice->begin(this);
    devices.push_back(newDevice);
  }
  return *newDevice;
}

__attribute__ ((deprecated("Please use DeviceType& myDevice = SinricPro.add<DeviceType>(DeviceId);")))
void SinricProClass::add(SinricProDeviceInterface* newDevice) {
  if (!checkDeviceId(String(newDevice->getDeviceId()))) return;
  newDevice->begin(this);
  devices.push_back(newDevice);
}

__attribute__ ((deprecated("Please use DeviceType& myDevice = SinricPro.add<DeviceType>(DeviceId);")))
void SinricProClass::add(SinricProDeviceInterface& newDevice) {
  if (!checkDeviceId(String(newDevice.getDeviceId()))) return;
  newDevice.begin(this);
  devices.push_back(&newDevice);
}

void SinricProClass::handle() {
  if (!isConnected()) connect();
  _websocketListener.handle();
  _udpListener.handle();

  handleRequest();
  handleSendQueue();
}

void SinricProClass::handleRequest() {
  if (receiveQueue.count() == 0) return;

  DEBUG_SINRIC("[SinricPro.handleRequest()]: %i items in receiveQueue\r\n", receiveQueue.count());
  // POP requests and call device.handle() for each related device
  while (receiveQueue.count() > 0) {  
    SinricProMessage* rawMessage = receiveQueue.pop();
    DynamicJsonDocument requestMessage(1024);
    deserializeJson(requestMessage, rawMessage->getMessage());
    // check signature
    bool sigMatch = verifyMessage(signingKey, requestMessage);
    DEBUG_SINRIC("[SinricPro.handleRequest()]: Signature is %s\r\n", sigMatch?"valid":"invalid");

    if (sigMatch) { // signature is valid }
      #ifndef NODEBUG_SINRIC  
              String debugOutput;
              serializeJsonPretty(requestMessage, debugOutput);
              DEBUG_SINRIC("%s\r\n", debugOutput.c_str());
      #endif

      DynamicJsonDocument responseMessage = prepareResponse(requestMessage);

      // handle devices
      bool success = false;
      const char* deviceId = requestMessage["payload"]["deviceId"];
      const char* action = requestMessage["payload"]["action"];
      JsonObject request_value = requestMessage["payload"]["value"];
      JsonObject response_value = responseMessage["payload"]["value"];
  
      for (auto& device : devices) {
        if (strcmp(deviceId, device->getDeviceId()) == 0 && success == false) {
          success = device->handleRequest(deviceId, action, request_value, response_value);
          responseMessage["payload"]["success"] = success;
        }
      }
      // sign message
      String responseString = signMessage(signingKey.c_str(), responseMessage);
      // debug output message
      #ifndef NODEBUG_SINRIC
              String responseStringPretty;
              serializeJsonPretty(responseMessage, responseStringPretty);
              DEBUG_SINRIC("[SinricPro.handleRequest()]: response:\r\n%s\r\n", responseStringPretty.c_str());
      #endif
      // push response message to sendQueue
      sendQueue.push(new SinricProMessage(rawMessage->getInterface(), responseString.c_str()));
    } else { // signature is invalid!
//      DEBUG_SINRIC("[SinricPro.handleRequest()]: Signature should be: %s\r\n", calculateSignature(_signingKey.c_str(), jsonMessage).c_str());
    }
    delete rawMessage;
  }
}

void SinricProClass::handleSendQueue() {
  if (!isConnected()) return;
  if (sendQueue.count() > 0) {
    DEBUG_SINRIC("[SinricPro:handleSendQueue()]: %i item(s) in sendQueue\r\n", sendQueue.count());
    SinricProMessage* rawMessage = sendQueue.pop();
//    DEBUG_SINRIC("[SinricPro:handleSendQueue()]:\r\n%s\r\n", rawMessage->getMessage());
    switch (rawMessage->getInterface()) {
      case IF_WEBSOCKET: _websocketListener.sendResponse(rawMessage->getMessage()); break;
      case IF_UDP:       _udpListener.sendResponse(rawMessage->getMessage()); break;
      default:           break;
    }
    delete rawMessage;
  }
}

void SinricProClass::connect() {
  String deviceList;
  int i = 0;
  for (auto& device : devices) {
    if (i>0) deviceList += ";";
    deviceList += String(device->getDeviceId());
    i++;
  }

  _websocketListener.begin(serverURL, socketAuthToken, deviceList.c_str(), &receiveQueue);
}


void SinricProClass::stop() {
  DEBUG_SINRIC("[SinricPro:stop()\r\n");
  _websocketListener.disconnect();
  _websocketListener.stop();
}

bool SinricProClass::isConnected() {
  return _websocketListener.isConnected();
};


void SinricProClass::reconnect() {
  DEBUG_SINRIC("SinricProClass.reconnect(): disconnecting\r\n");
  stop();
  DEBUG_SINRIC("SinricProClass.reconnect(): wait 1second\r\n");
  delay(1000);
  DEBUG_SINRIC("SinricProClass.reconnect(): connecting\r\n");
  connect();
}

bool SinricProClass::checkDeviceId(String deviceId) {
  if (deviceId.length() != 24) {
    DEBUG_SINRIC("[SinricPro.add()]: Invalid deviceId \"%s\"! Device will be ignored!\r\n", deviceId.c_str());
    return false;
  }

  for (size_t i = 0; i < deviceId.length(); i++) {
    char current = deviceId[i];
    if (current >= '0' && current <= '9') continue;
    if (current >= 'A' && current <= 'F') continue;
    if (current >= 'a' && current <= 'f') continue;
    DEBUG_SINRIC("[SinricPro.add()]: Invalid deviceId \"%s\"! Device will be ignored!\r\n", deviceId.c_str());
    return false;
  }
  return true;
}


void SinricProClass::sendMessage(JsonDocument& jsonMessage) {
  String messageString = signMessage(signingKey, jsonMessage);
  sendQueue.push(new SinricProMessage(IF_WEBSOCKET, messageString.c_str()));
  #ifndef NODEBUG_SINRIC
          String debugOutput;
          serializeJsonPretty(jsonMessage, debugOutput);
          DEBUG_SINRIC("Signed event:\r\n%s\r\n", debugOutput.c_str());
  #endif
}


DynamicJsonDocument SinricProClass::prepareResponse(JsonDocument& requestMessage) {
  DynamicJsonDocument responseMessage(1024);
  JsonObject header = responseMessage.createNestedObject("header");
  header["payloadVersion"] = 2;
  header["signatureVersion"] = 1;

  JsonObject payload = responseMessage.createNestedObject("payload");
  payload["action"] = requestMessage["payload"]["action"];
  payload["clientId"] = requestMessage["payload"]["clientId"];
  payload["createdAt"] = _ntp.getTimestamp();
  payload["deviceId"] = requestMessage["payload"]["deviceId"];
  payload["message"] = "OK";
  payload["replyToken"] = requestMessage["payload"]["replyToken"];
  payload["success"] = false;
  payload["type"] = "response";
  payload.createNestedObject("value");
  return responseMessage;
}


DynamicJsonDocument SinricProClass::prepareEvent(const char* deviceId, const char* action, const char* cause) {
  DynamicJsonDocument eventMessage(1024);
  JsonObject header = eventMessage.createNestedObject("header");
  header["payloadVersion"] = 2;
  header["signatureVersion"] = 1;

  JsonObject payload = eventMessage.createNestedObject("payload");
  payload["action"] = action;
  payload["cause"] = cause;
  payload["createdAt"] = _ntp.getTimestamp();
  payload["deviceId"] = deviceId;
  payload["replyToken"] = MessageID().getID();
  payload["type"] = "event";
  payload.createNestedObject("value");
  return eventMessage;
}

#ifndef NOSINRIC_INSTANCE 
SinricProClass SinricPro;
#endif

#endif