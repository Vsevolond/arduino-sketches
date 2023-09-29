#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>

#include <ArduinoJson.h>

#include <ESP8266WiFi.h>
#include <AsyncMqttClient.h>
#include <EEPROM.h>

#include <Wire.h>                                    
#include <Adafruit_BME280.h>                            
#include <Adafruit_Sensor.h>                            
 
#define SEALEVELPRESSURE_HPA (1013.25) 

#define IP(a,b,c,d) (uint32_t)(a | (b << 8) | (c << 16) | (d << 24))
#define MAX_NETWORKS 100

AsyncWebServer server(80);

const uint32_t apIP = IP(192, 168, 200, 1);
const uint32_t apGateway = apIP;
const uint32_t apSubnet = IP(255, 255, 255, 0);

String deviceID;
int statusConnect;
String deviceType = "Air Controller";
bool doesAppKnow = false;
String ssid = "";
String password = "";

const char* mqtt_server = "test.mosquitto.org";
const int mqtt_port = 1883;

WiFiClient espClient;
AsyncMqttClient mqtt;

String pingTopic, pongTopic;
String temperatureTopic, humidityTopic, pressureTopic, heightTopic;
 
Adafruit_BME280 bme; 

byte countOfActiveTopics = 0;

enum device_mode { 
  CHECKING,
  STARTED,
  CONFIGURE,
  READY,
  CONNECTED,
  WAITING,
  DISCONNECTED,
} deviceMode;

struct Device {
  float temperature;
  float humidity;
  float pressure;
  float height;
};

Device device;

 
void setup() {
  Serial.begin(115200);

  deviceMode = DISCONNECTED;
  statusConnect = WL_DISCONNECTED;

  startAP();                            
}

void startAP() {
  String unique = String(ESP.getChipId(), HEX);
  unique.toUpperCase();
  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  Serial.println("Setting AP...");
  WiFi.softAPConfig(IPAddress(apIP), IPAddress(apGateway), IPAddress(apSubnet));
  Serial.println(WiFi.softAP("Device_" + unique, "", 11, false, 1) ? "ready" : "failed");
  IPAddress IP = WiFi.softAPIP();
  WiFi.scanNetworks(true);
  Serial.print("softAP IP address: ");
  Serial.println(IP);
  deviceID = "device_" + unique;

  startServer();
}

void connectToNetwork() {
  WiFi.mode(WIFI_AP_STA);
  delay(100);
  WiFi.begin(ssid, password);
  while (WiFi.status() == WL_DISCONNECTED) {
    Serial.println("Connecting...");
    delay(1000);
  }
  statusConnect = WiFi.status();
  if (statusConnect != WL_CONNECTED) {
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    delay(100);
  }
  Serial.println(statusConnect);
}

void stopConfigure() {
    doesAppKnow = false;
    server.end();
    WiFi.mode(WIFI_STA);
    delay(100);
    
    Serial.print("Connected to WiFi: ");
    Serial.println(WiFi.SSID());
    Serial.print("With IP: ");
    Serial.println(WiFi.localIP());
    
    connectToMqtt();
    while (!mqtt.connected()) {
      Serial.println("Connecting to MQTT...");
      delay(1000);
    }

    String unique = String(ESP.getChipId(), HEX);
    unique.toUpperCase();

    pingTopic = "device_" + unique + "/ping";
    pongTopic = "device_" + unique + "/pong";
    
    temperatureTopic = "device_" + unique + "/temperature";
    humidityTopic = "device_" + unique + "/humidity";
    pressureTopic = "device_" + unique + "/pressure";
    heightTopic = "device_" + unique + "/height";
  
    subscribeTo(pingTopic);
                                                           
    if (!bme.begin(0x76)) {                         
      Serial.println("Could not find a valid BME280 sensor, check wiring!");                                  
    } 
}

void startServer() {
  server.on("/device/ping", HTTP_GET, [](AsyncWebServerRequest *request) {
    String response;
    const size_t capacity = JSON_OBJECT_SIZE(4);
    DynamicJsonDocument jsonDocument(capacity);
    jsonDocument["id"] = deviceID;
    jsonDocument["type"] = deviceType;
    serializeJsonPretty(jsonDocument, response);
    request->send(200, "application/json", response);
  });
  
  server.on("/device/scanNetworks", HTTP_GET, [](AsyncWebServerRequest *request) {
      String response;
      int n = WiFi.scanComplete();
      if (n == -2) {
        WiFi.scanNetworks(true);
      } else {
        const size_t capacity = JSON_ARRAY_SIZE(MAX_NETWORKS) + MAX_NETWORKS*JSON_OBJECT_SIZE(2);
        DynamicJsonDocument jsonDocument(capacity);
        for (int i = 0; i < n; i++) {
          JsonObject wifi = jsonDocument.createNestedObject();
          wifi["ssid"] = WiFi.SSID(i);
          wifi["isOpen"] = WiFi.encryptionType(i) == ENC_TYPE_NONE;
        }
        WiFi.scanDelete();
        if(WiFi.scanComplete() == -2) {
          WiFi.scanNetworks(true);
        }
        serializeJsonPretty(jsonDocument, response);
      }
      request->send(200, "application/json", response);
  });

  server.on("/device/configureNetwork", HTTP_POST, [](AsyncWebServerRequest *request) {
      String json = request->getParam(0)->value().c_str();
      size_t capacity = JSON_OBJECT_SIZE(5);
      DynamicJsonDocument doc(capacity);
      deserializeJson(doc, json);
      const char* network = doc["ssid"];
      const char* pass = doc["password"];
      ssid = String(network);
      password = String(pass);

      String response;
      capacity = JSON_OBJECT_SIZE(2);
      DynamicJsonDocument jsonDocument(capacity);
      statusConnect = WL_IDLE_STATUS;
      jsonDocument["status"] = statusConnect;
      serializeJsonPretty(jsonDocument, response);
      request->send(200, "application/json", response);
  });

  server.on("/device/statusConnect", HTTP_GET, [](AsyncWebServerRequest *request) {
      String response;
      const size_t capacity = JSON_OBJECT_SIZE(2);
      DynamicJsonDocument jsonDocument(capacity);
      jsonDocument["status"] = statusConnect;
      serializeJsonPretty(jsonDocument, response);
      if (statusConnect == WL_CONNECTED) {
        doesAppKnow = true;
      }
      request->send(200, "application/json", response);
  });

  server.begin();
}

void messageCallback(String topic, String message) {
                        
  if (topic == pingTopic) {
    
    if (message == "ping") {

      if (deviceMode == DISCONNECTED) {
        setupMode(WAITING);
      } else if (deviceMode == READY || deviceMode == STARTED) {
        setupMode(CHECKING);
      } else if (deviceMode == WAITING) {
        setupMode(WAITING);
      }
      
    } else if (message == "ready") {
      
      if (deviceMode == CONNECTED || deviceMode == CHECKING || deviceMode == READY || deviceMode == STARTED) {
        setupMode(READY);
      } else if (deviceMode == WAITING) {
        setupMode(CONNECTED); 
      }
    } else if (message == "status") {

      if (deviceMode == READY || deviceMode == STARTED) {
        setupMode(CONFIGURE);
      }
    } else {
      Serial.println("Unexpected message: " + message + " ### in topic: " + topic);
    }
  }
}

void subscribeCallback() {
  countOfActiveTopics++;
}

void unsubscribeCallback() {
  countOfActiveTopics--;
}


void setupMode(device_mode mode_t) {
  deviceMode = mode_t;
  
  switch (deviceMode) {

    case CHECKING:

      publishTo(pongTopic, "pong");
      break;

    case STARTED:
      break;

    case CONFIGURE:
    {
      sendStatus();
      setupMode(STARTED);
    }
      break;
      
    case READY:
    
      publishTo(pongTopic, "ready");
      break;
      
    case CONNECTED:
    
      setupMode(READY);
      break;
      
    case WAITING:
    
      publishTo(pongTopic, "pong");
      break;
      
    case DISCONNECTED:
      break;
  }
}


void sendStatus() {
  String status;
  
  device.temperature = bme.readTemperature();
  status += String(device.temperature) + "#";

  device.humidity = bme.readHumidity();
  status += String(device.humidity) + "#";

  device.pressure = bme.readPressure() / 100.0F;
  status += String(device.pressure) + "#";

  device.height = bme.readAltitude(SEALEVELPRESSURE_HPA);
  status += String(device.height);
  
  publishTo(pongTopic, status);
}
 
void loop() {
  if (statusConnect == WL_IDLE_STATUS && ssid.length() > 0 && password.length() > 0) {
    connectToNetwork();
  }

  if (statusConnect == WL_CONNECTED && doesAppKnow) {
    stopConfigure();
  }
  
  if (deviceMode == STARTED) {
    
    float temperature = bme.readTemperature();
    if (device.temperature != temperature) {
       device.temperature = temperature;
       publishTo(temperatureTopic, String(temperature));
    }

    float humidity = bme.readHumidity();
    if (device.humidity != humidity) {
      device.humidity = humidity;
      publishTo(humidityTopic, String(humidity));
    }

    float pressure = bme.readPressure() / 100.0F;
    if (device.pressure != pressure) {
      device.pressure = pressure;
      publishTo(pressureTopic, String(pressure));
    }

    float height = bme.readAltitude(SEALEVELPRESSURE_HPA);
    if (device.height != height) {
      device.height = height;
      publishTo(heightTopic, String(height));
    }
  }
}



// --------------- MQTT ------------------------------------


void subscribeTo(String topic) {
  mqtt.subscribe(topic.c_str(), 1);
}

void unsubscribeFrom(String topic) {
  mqtt.unsubscribe(topic.c_str());
}

void publishTo(String topic, String message) {
  mqtt.publish(topic.c_str(), 1, true, message.c_str());
}


void connectToMqtt() {
  mqtt.onConnect(onMqttConnect);
  mqtt.onMessage(onMqttMessage);
  mqtt.onSubscribe(onMqttSubscribe);
  mqtt.onUnsubscribe(onMqttUnsubscribe);
  mqtt.onDisconnect(onMqttDisconnect);
  
  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.connect();
}


void onMqttConnect(bool sessionPresent) {
  Serial.println("Connected to MQTT.");
  Serial.print("Session present: ");
  Serial.println(sessionPresent);
}


void onMqttMessage(char* topic_t, char* payload, 
               AsyncMqttClientMessageProperties properties, 
               size_t len, size_t index, size_t total) {
                
  String topic = String(topic_t);
  Serial.print("Message received in topic: ");
  Serial.print(topic);
  Serial.print(" ### Message: ");
  String message;
  for (int i = 0; i < len; i++) 
  {
    message = message + (char)payload[i];
  }
  Serial.println(message);

  messageCallback(topic, message);
}

void onMqttSubscribe(uint16_t packetId, uint8_t qos) {
  subscribeCallback();
}

void onMqttUnsubscribe(uint16_t packetId) {
  unsubscribeCallback();
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  Serial.println("Disconnected from MQTT.");
}
