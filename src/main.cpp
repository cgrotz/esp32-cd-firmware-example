#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <HTTPClient.h>
#include <Stream.h>
#include <StreamString.h>
#include <MQTT.h>

String INSTALLATION_PATH = String("/features/softwareupdatable/inbox/messages/install");

const char ssid[] = "<Wifi SSID>";
const char pass[] = "<Wifi Password>";

// Connetion parameters from the provisioning json file
const char hubNamespace[] = "<the namespace we created>";
const char deviceId[] = "<deviceId>";
const char tenantId[] = "<iot hub tenant>";
const char mqttPassword[] = "<Wifi Password>";

// Since Bosch IoT Hub needs TLS, we need to use the WifiClient Secure instead of the normal Wifi Client
WiFiClientSecure net;
// We need to increase message size since the IoT Suite messages can be rather big
MQTTClient client(2048);

unsigned long lastMillis = 0;

int installationState = 0;
StaticJsonDocument<2000> installationCommand;

boolean restart = false;

String buildInstallationState(String correlationId, String moduleName, 
    String version, String state) {
  char payload[1000];
  sprintf(payload, "{\r\n\t\"topic\": \"%s/%s/things/twin/commands/modify\",\r\n\t\"path\": \"/features/%s\",\r\n\t\"value\": {\r\n\t \t\"definition\": [\r\n\t \t\t\"org.eclipse.hawkbit.swmodule:SoftwareModule:1.0.0\"\r\n\t \t],\r\n \t \t\"properties\": {\r\n \t\t \t\"status\": {\r\n \t\t\t \t\"moduleName\" : \"%s\",\r\n \t\t\t \t\"moduleVersion\" : \"%s\",\r\n \t\t\t \t\"status\" : {\r\n\t\t\t\t\t\"correlationId\": \"%s\",\r\n\t\t\t\t\t\"operation\": \"install\",\r\n\t\t\t\t\t\"status\": \"%s\"\r\n\t\t\t\t}\r\n\t\t\t}\r\n\t\t}\r\n\t}\r\n}", hubNamespace, deviceId, moduleName, moduleName, version, correlationId, state);
  return payload;
}

String buildInstallationState(String correlationId, String moduleName, 
    String version, String state, String message) {
  char payload[1000];
  sprintf(payload, "{\r\n\t\"topic\": \"%s/%s/things/twin/commands/modify\",\r\n\t\"path\": \"/features/%s\",\r\n\t\"value\": {\r\n\t \t\"definition\": [\r\n\t \t\t\"org.eclipse.hawkbit.swmodule:SoftwareModule:1.0.0\"\r\n\t \t],\r\n \t \t\"properties\": {\r\n \t\t \t\"status\": {\r\n \t\t\t \t\"moduleName\" : \"%s\",\r\n \t\t\t \t\"moduleVersion\" : \"%s\",\r\n \t\t\t \t\"status\" : {\r\n\t\t\t\t\t\"correlationId\": \"%s\",\r\n\t\t\t\t\t\"operation\": \"install\",\r\n\t\t\t\t\t\"status\": \"%s\",\r\n\t\t\t\t\t\"message\": \"%s\r\n\t\t\t\t}\r\n\t\t\t}\r\n\t\t}\r\n\t}\r\n}", hubNamespace, deviceId, moduleName, moduleName, version, correlationId, state, message);
  return payload;
}

void messageReceived(String &mqttTopic, String &payload) {
  StaticJsonDocument<2000> doc;
  deserializeJson(doc, payload);
  String topic = doc["topic"];
  String path = doc["path"];

  Serial.print("incoming topic: ");
  Serial.print(topic);
  Serial.print(" path: ");
  Serial.println(path);
  Serial.println("incoming: " + mqttTopic + " - " + payload);
  
  // Note: Do not use the client in the callback to publish, subscribe or
  // unsubscribe as it may cause deadlocks when other things arrive while
  // sending and receiving acknowledgments. Instead, change a global variable,
  // or push to a queue and handle it in the loop after calling `client.loop()`.
}

void connect() {
  Serial.print("\nchecking wifi...");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(1000);
  }
  Serial.print(" connected to wifi!");

  Serial.print("\nconnecting to mqtt...");
  client.begin("mqtt.bosch-iot-hub.com", 8883, net);
  client.onMessage(messageReceived);

  int wait = 0;
  char mqttDeviceId[100];
  char mqttUsername[100];
  sprintf(mqttDeviceId, "%s:%s", hubNamespace, deviceId);
  sprintf(mqttUsername, "%s_%s@%s", hubNamespace, deviceId, tenantId);
  while (!client.connect(mqttDeviceId, mqttUsername,mqttPassword, false) && wait < 10) {
    Serial.print(".");
    delay(1000);
    wait++;
  }
  if(!client.connected()) {
    Serial.println(" not connected to mqtt!");
  }
  else {
    Serial.println(" connected to mqtt!");
  }
  // Subscribe to commands from IoT Suite
  client.subscribe("command///req/#");

  // Update Thing Device Shadow with 
  char connectPayload[1000];
  sprintf(connectPayload, "{\r\n\t\"topic\": \"%s/%s/things/twin/commands/modify\",\r\n\t\"path\": \"/features/softwareupdatable\",\r\n\t\"value\": {\r\n\t \t\"definition\": [\r\n\t \t\t\"org.eclipse.hawkbit.swupdatable:SoftwareUpdatable:1.0.0\"\r\n\t \t],\r\n \t \t\"properties\": {\r\n \t\t \t\"status\": {\r\n \t\t\t \t\"agentName\" : \"m5stack\",\r\n \t\t\t \t\"agentVersion\" : \"1.0.0\",\r\n \t\t\t \t\"type\" : \"application\"\r\n\t\t\t}\r\n\t\t}\r\n\t}\r\n}",hubNamespace, deviceId);
  if(!client.publish("event", connectPayload, false, 1)) {
    Serial.print("Failed publishing ");
    Serial.println(client.lastError());
  }
}


void downloadAndInstallFirmware(String correlationId, String module, 
    String version, String url) {
  WiFiClientSecure net2;
  Serial.printf("[HTTPS] Downloading Firmware %s\n", url.c_str());
  HTTPClient https;
  if (https.begin(net2, url.c_str())) {
    int httpCode = https.GET();
    // httpCode will be negative on error
    if (httpCode > 0) {
      // HTTP header has been send and Server response header has been handled
      Serial.printf("[HTTPS] GET... code: %d\n", httpCode);

      // file found at server
      if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
        client.publish("event", buildInstallationState(correlationId, module, version, "INSTALLING"), false, 1);
        bool canBegin = Update.begin(https.getSize());
        if(canBegin) {
          Update.writeStream(https.getStream());

          if (!client.connected()) {
            connect();
          }
          if (Update.end()) {
            Serial.println("OTA done!");
            client.publish("event", buildInstallationState(correlationId, module, version, "INSTALLED"), false, 1);
            if (Update.isFinished()) {
              client.publish("event", buildInstallationState(correlationId, module, version, "FINISHED_SUCCESS"), false, 1);
              restart = true;
            } else {
              client.publish("event", buildInstallationState(correlationId, module, version, "FINISHED_ERROR"), false, 1);
              Serial.println("Update not finished? Something went wrong!");
            }
          } else {
              client.publish("event", buildInstallationState(correlationId, module, version, "FINISHED_ERROR", "Error Occurred. Error #: " + String(Update.getError())), false, 1);
            Serial.println("Error Occurred. Error #: " + String(Update.getError()));
          }
        }
      }
    } else {
      Serial.printf("[HTTPS] GET... failed, error: %s\n", https.errorToString(httpCode).c_str());
    }

    https.end();
  }
  else {
    Serial.println("Failed downloading");
  }
}

void setup() {
  Serial.begin(115200);
  WiFi.begin(ssid, pass);
  connect();
}

void loop() {
  client.loop();
  delay(10);  // we add a few millisecond delay for wifi stability

  if (!client.connected()) {
    connect();
  }

  if(restart) {
    ESP.restart();
  }

  // publish a message roughly every second.
  if (millis() - lastMillis > 1000) {
    lastMillis = millis();
    char payload[1000];
    sprintf(payload, "{\r\n\t\"topic\": \"%s/%s/things/twin/commands/modify\",\r\n\t\"path\": \"/features/version\",\r\n\t\"value\": {\r\n\t \t\"properties\": {\r\n \t\t \t\"version\": \"1.0.8\"\r\n\t\t}\r\n\t}\r\n}",hubNamespace, deviceId );
    if(!client.publish("event", payload, false, 1)) {
      Serial.print("Failed publishing ");
      Serial.println(client.lastError());
    }
  }

  if(installationState != 0) {
    const char* correlationId = installationCommand["value"]["arg_0"]["correlationId"];
    JsonArray softwareModules = installationCommand["value"]["arg_0"]["softwareModules"].as<JsonArray>();
    for(JsonVariant v : softwareModules) {
      JsonObject moduleDoc = v.as<JsonObject>();
      String module = moduleDoc["name"];
      String version = moduleDoc["version"];
      Serial.print("Installing: ");
      Serial.print(module);
      Serial.print(" version: ");
      Serial.println(version);
      Serial.print(" correlationId: ");
      Serial.println(correlationId);

      client.publish("event", buildInstallationState(correlationId, module, version, "STARTED"), false, 1);
  
      JsonArray artifacts = moduleDoc["artifacts"].as<JsonArray>();
      for(JsonVariant artifactVariant : artifacts) {
        JsonObject artifact = artifactVariant.as<JsonObject>();
        String filename = artifact["filename"];
        String md5 = artifact["hashes"]["md5"];
        String url = artifact["links"]["download"]["https"];
        // TODO check if filename ends with bin
        Serial.print("Artifact: ");
        Serial.print(filename);
        Serial.print(" md5: ");
        Serial.println(md5);

        downloadAndInstallFirmware(correlationId, module, version, url);
      }
    }
    installationState = 0;
  }
}