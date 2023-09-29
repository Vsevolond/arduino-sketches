#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>

#include <ArduinoJson.h>
//#include <Dictionary.h>

#include <ESP8266WiFi.h>
#include <AsyncMqttClient.h>
#include <EEPROM.h>

#define LED D5
#define COLOR_RED D6
#define COLOR_GREEN D7
#define COLOR_BLUE D8

#define IP(a,b,c,d) (uint32_t)(a | (b << 8) | (c << 16) | (d << 24))
#define MAX_NETWORKS 100

//const char* ssid = "RT-WiFi-EA4D";
//const char* password =  "qufA9aENPu";
String ssid = "";
String password = "";

const char* mqtt_server = "test.mosquitto.org";
const int mqtt_port = 1883;

WiFiClient espClient;
AsyncMqttClient mqtt;

AsyncWebServer server(80);

const uint32_t apIP = IP(192, 168, 200, 1);
const uint32_t apGateway = apIP;
const uint32_t apSubnet = IP(255, 255, 255, 0);

String deviceID;
String deviceType = "Smart Lightning";
int statusConnect;
bool doesAppKnow = false;

String pingTopic, pongTopic;
String stateTopic, modeTopic, brightTopic, colorTopic;

struct Device {
  bool state = false;
  byte bright = 100;
  char color[7] = "FFFFFF";
  byte mode = 0; // 0 - light, 1 - multicolor, 2 - rainbow
};

bool isRainbow = false;

int offset = 0;
unsigned long lastTime;
unsigned long currentTime;

Device device;

byte countOfActiveTopics = 0;

enum device_mode { 
  CHECKING,
  READY,
  CONNECTED,
  SUBSCRIBING,
  WAITING,
  DISCONNECTED,
} deviceMode;



void setup() {
  Serial.begin(115200);
  EEPROM.begin(255);
  
  pinMode(LED, OUTPUT);
  pinMode(COLOR_RED, OUTPUT);
  pinMode(COLOR_GREEN, OUTPUT);
  pinMode(COLOR_BLUE, OUTPUT);
  digitalWrite(LED, LOW);
  digitalWrite(COLOR_RED, LOW);
  digitalWrite(COLOR_GREEN, LOW);
  digitalWrite(COLOR_BLUE, LOW);

  lastTime = millis();
  deviceMode = DISCONNECTED;
  statusConnect = WL_DISCONNECTED;

  startAP();
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
  
    EEPROM.get(0, device);
    setupDevice();
  
    pingTopic = "device_" + unique + "/ping";
    pongTopic = "device_" + unique + "/pong";
    
    stateTopic = "device_" + unique + "/state";
    brightTopic = "device_" + unique + "/brightness";
    colorTopic = "device_" + unique + "/color";
    modeTopic = "device_" + unique + "/mode";
  
    subscribeTo(pingTopic);
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

int hexToInt(char hexDigit1, char hexDigit2) {
  int num = 0;
  int digit1 = hexDigit1, digit2 = hexDigit2;
  
  if (digit1 >= 48 && digit1 <= 57) {
    digit1 -= '0';
  } else if (digit1 >= 65 && digit1 <= 70) {
    digit1 -= ('A' - 10);
  }

  if (digit2 >= 48 && digit2 <= 57) {
    digit2 -= '0';
  } else if (digit2 >= 65 && digit2 <= 70) {
    digit2 -= ('A' - 10);
  }

  num = digit1 * 16  + digit2;
  return num;
}

void setupDevice() {
  if (device.state) {
    switch (device.mode) {
      case 0:
      {
        isRainbow = false;
        digitalWrite(COLOR_RED, LOW);
        digitalWrite(COLOR_GREEN, LOW);
        digitalWrite(COLOR_BLUE, LOW);
        
        analogWrite(LED, map(device.bright, 0, 255, 0, 1023));
      }
        break;
      case 1:
        {
          isRainbow = false;
          digitalWrite(LED, LOW);
          
          int red = hexToInt(device.color[0], device.color[1]);
          int green = hexToInt(device.color[2], device.color[3]);
          int blue = hexToInt(device.color[4], device.color[5]);

          analogWrite(COLOR_RED, map(red, 0, 255, 0, 1023));
          analogWrite(COLOR_GREEN, map(green, 0, 255, 0, 1023));
          analogWrite(COLOR_BLUE, map(blue, 0, 255, 0, 1023));
        }
        break;
      case 2:
        {
          digitalWrite(LED, LOW);
          isRainbow = true;
        }
        break;
    }
  } else {
    digitalWrite(LED, LOW);
    digitalWrite(COLOR_RED, LOW);
    digitalWrite(COLOR_GREEN, LOW);
    digitalWrite(COLOR_BLUE, LOW);
  }
}


void messageCallback(String topic, String message) {
                        
  if (topic == pingTopic) {
    
    if (message == "ping") {

      if (deviceMode == DISCONNECTED) {
        setupMode(WAITING);
      } else if (deviceMode == READY) {
        setupMode(CHECKING);
      } else if (deviceMode == WAITING) {
        setupMode(WAITING);
      }
      
    } else if (message == "ready") {
      
      if (deviceMode == CONNECTED || deviceMode == CHECKING || deviceMode == READY) {
        setupMode(READY);
      } else if (deviceMode == WAITING) {
        setupMode(SUBSCRIBING); 
      }
    } else if (message == "status") {

      if (deviceMode == READY) {
        sendStatus();
      }
    } else {
      Serial.println("Unexpected message: " + message + " ### in topic: " + topic);
    }
    
  } else if (deviceMode == READY) {
  
    if (topic == stateTopic) {
      handleState(message);
    } else if (topic == brightTopic) {
      handleBright(message);
    } else if (topic == colorTopic) {
      handleColor(message);
    } else if (topic == modeTopic) {
      handleMode(message);
    } else {
      Serial.println("Unexpected topic: " + topic + " ### in mode: READY");
    }
    
  } else {
    
    if (topic == stateTopic || topic == brightTopic || topic == colorTopic || topic == modeTopic) {
      Serial.println("Device not ready!");
    } else {
      Serial.println("Unexpected topic: " + topic);
    }
    
  }
 
}


void subscribeCallback() {
  countOfActiveTopics++;

  if (isSubscribed()) {
    setupMode(CONNECTED);
  }
}

bool isSubscribed() {
  return countOfActiveTopics == 5;
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
      
    case READY:
    
      publishTo(pongTopic, "ready");
      break;
      
    case CONNECTED:
    
      setupMode(READY);
      break;
      
    case SUBSCRIBING:
    
      subscribeToTopics();
      break;
      
    case WAITING:
    
      publishTo(pongTopic, "pong");
      break;
      
    case DISCONNECTED:
    
      unsubscribeFromTopics();
      break;
  }
}


void subscribeToTopics() {
  subscribeTo(stateTopic);
  subscribeTo(brightTopic);
  subscribeTo(colorTopic);
  subscribeTo(modeTopic);
}

void unsubscribeFromTopics() {

  unsubscribeFrom(stateTopic);
  unsubscribeFrom(brightTopic);
  unsubscribeFrom(colorTopic);
  unsubscribeFrom(modeTopic);
  
}


void sendStatus() {
  String status;
  String state = (device.state == 1 ? "on" : "off");
  status += state + "#";
  String mode;
  switch (device.mode) {
    case 0:
      mode = "light";
      break;
    case 1:
      mode = "multicolor";
      break;
    case 2:
      mode = "rainbow";
      break;
  }
  status += mode + "#";
  status += String(device.bright) + "#";
  status += String(device.color);
  publishTo(pongTopic, status);
}


void handleState(String message) {
  if (message == "off" || message == "on") {
    
    device.state = (message == "on" ? 1 : 0);
    setupDevice();
    
    EEPROM.put(0, device);
    EEPROM.commit();
    
  } else {
    Serial.println("Unexpected message: " + message + " ### in topic: " + stateTopic);
  }
}

void handleMode(String message) {
  byte mode;
  if (message == "light") {
    mode = 0;
  } else if (message == "multicolor") {
    mode = 1;
  } else if (message == "rainbow") {
    mode = 2;
  } else {
    mode = -1;
  }

  if (mode != -1) {
    device.mode = mode;
    setupDevice();

    EEPROM.put(0, device);
    EEPROM.commit();
  } else {
    Serial.println("Unexpected message: " + message + " ### in topic: " + modeTopic);
  }
}


void handleBright(String message) {
  if (isNumber(message)) {
    
    byte new_bright = message.toInt();
    if (new_bright >= 0 && new_bright <= 255) {
      
      device.bright = new_bright;
      setupDevice();
      
      EEPROM.put(0, device);
      EEPROM.commit();
      
    }
  } else {
    Serial.println("Unexpected message: " + message + " ### in topic: " + brightTopic);
  }
}

void handleColor(String message) {
  strcpy(device.color, message.c_str());
  setupDevice();

  EEPROM.put(0, device);
  EEPROM.commit();
}


bool isNumber(String str) {
  bool isNum = true;
  for (char x : str) {
    isNum = isNum && isDigit(x);
  }

  return isNum;
}

void showRGB(int color) {
  int redIntensity;
  int greenIntensity;
  int blueIntensity;
    
  if (color <= 255) {
    redIntensity = 255 - color;    // red goes from on to off
    greenIntensity = color;        // green goes from off to on
    blueIntensity = 0;             // blue is always off
  } else if (color <= 511) {
    redIntensity = 0;                     // red is always off
    greenIntensity = 255 - (color - 256); // green on to off
    blueIntensity = (color - 256);        // blue off to on
  } else { // color >= 512
    redIntensity = (color - 512);         // red off to on
    greenIntensity = 0;                   // green is always off
    blueIntensity = 255 - (color - 512);  // blue on to off
  }
  
  analogWrite(COLOR_RED, redIntensity);
  analogWrite(COLOR_GREEN, greenIntensity);
  analogWrite(COLOR_BLUE, blueIntensity);
}

void loop() {
  if (statusConnect == WL_IDLE_STATUS && ssid.length() > 0 && password.length() > 0) {
    connectToNetwork();
  }

  if (statusConnect == WL_CONNECTED && doesAppKnow) {
    stopConfigure();
  }

  if (deviceMode == READY) {

    currentTime = millis();
    
    if (device.state && isRainbow && (currentTime - lastTime) > 10) {
      offset++;
      offset = offset % 768;
      lastTime = currentTime;
      showRGB(offset);
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
