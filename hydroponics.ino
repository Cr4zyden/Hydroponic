#include <ESP8266WiFi.h>
#include <PubSubClient.h>

#include <time.h>
#include <Wire.h>
#include <microDS3231.h>
MicroDS3231 rtc;

#include <WiFiUdp.h>
WiFiUDP ntpUDP;

#include <NTPClient.h>
const long utcOffsetInSeconds = 25200;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds);

#include <TZ.h>
#include <FS.h>
#include <LittleFS.h>
#include <CertStoreBearSSL.h>
#include <GyverOS.h>
GyverOS<2> OS;

// Update these with values suitable for your network.
#define lamp 21
#define pomp 16

const char* ssid = "110";
const char* password = "987654321";
const char* mqtt_server = "6a6cabe58a554338bd7bfb2cc39cf4b0.s1.eu.hivemq.cloud";
const char* mqtt_name = "CityFarm";
const char* mqtt_password = "12345678";


bool firstBoot = true;
bool useMQTT = false;
bool WiFiConnected = false;

int watering_hours = 0;
int watering_minutes = 0;
int watering_start_time = 0;
const int watering_time = 20;
bool pomp_status = false;


struct tm timeinfo;

// A single, global CertStore which can be used by all connections.
// Needs to stay live the entire time any of the WiFiClientBearSSLs
// are present.
BearSSL::CertStore certStore;

WiFiClientSecure espClient;
PubSubClient * client;
unsigned long lastMsg = 0;
#define MSG_BUFFER_SIZE (500)
char msg[MSG_BUFFER_SIZE];
int value = 0;

void setup_wifi() {
  delay(10);
  // We start by connecting to a WiFi network
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  randomSeed(micros());

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
  //client->subscribe("esp");
}
time_t now = time(nullptr);

void setDateTime() {
  // You can use your own timezone, but the exact time is not used at all.
  // Only the date is needed for validating the certificates.
  configTime(TZ_Asia_Novosibirsk, "pool.ntp.org", "time.nist.gov");

  Serial.print("Waiting for NTP time sync: ");
  now = time(nullptr);
  gmtime_r(&now, &timeinfo);
  //Serial.println(now);
  while (now < 8 * 3600 * 2) {
    delay(100);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println();


  Serial.printf("%s", asctime(&timeinfo));
}

void callback(char* Topic, byte* Payload, unsigned int length) {
  Serial.println(Topic);
  if (strcmp(Topic, "CityFarm/commands/light") == 0) { // Сравниваем название топика
    char command = (char)Payload[0];
    Serial.println(*Payload);
    switch (command) {
      case '1': // Если полученная комманда 1, то включаем свет
        digitalWrite(LED_BUILTIN, LOW);
        Serial.println("Light on");
        digitalWrite(lamp, HIGH);
        break;
      case '0': //Если отправленная команда 0, то выключаем свет
        digitalWrite(LED_BUILTIN, HIGH);
        Serial.println("Light off");
        digitalWrite(lamp, LOW);
        break;
    }
  }
  /*
    if(strcmp(Topic,"Hydroponic/settings/watering_time")==0){
     Serial.println(length);
     watering_hours = (int)Payload[1];//*((int*)Payload);
     watering_minutes = (int)Payload[0];
     Serial.print(watering_hours);Serial.print(watering_minutes);
    }
  */
  if (strcmp(Topic, "CityFarm/settings/watering_time") == 0) {
    Serial.println(length);
    watering_hours = (int)(Payload[0] - 48) * 10 + (Payload[1] - 48); //*((int*)Payload);

    watering_minutes = (int)(Payload[3] - 48) * 10 + (Payload[4] - 48);
    Serial.print(watering_hours); Serial.print(watering_minutes);
  }
}



void watering() {
  switch (WiFiConnected) {
    case 1:
      if (watering_hours  == timeClient.getHours() & watering_minutes == timeClient.getMinutes()
          & not pomp_status & timeClient.getSeconds() < 10) {
        watering_start_time = timeClient.getSeconds();
        digitalWrite(pomp, HIGH);
        pomp_status = true;
        Serial.println(timeClient.getSeconds());
        Serial.println("Watering starts");
      }
      break;
    case 0:
      if (watering_hours  == rtc.getHours() & watering_minutes == rtc.getMinutes()
          & not pomp_status & rtc.getSeconds() < 10) {
        watering_start_time = rtc.getSeconds();
        digitalWrite(pomp, HIGH);
        pomp_status = true;
        Serial.println(rtc.getSeconds());
        Serial.println("Watering starts");
        break;
      }
  }

}

void checkConnections() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!WiFiConnected) {
      Serial.println("Connected to Wifi");
      WiFiConnected = true;
      timeClient.begin();
      timeClient.update();
      rtc.setTime(timeClient.getSeconds(), timeClient.getMinutes(), timeClient.getHours(), BUILD_DAY, BUILD_MONTH, BUILD_YEAR);
      Serial.println(timeClient.getMinutes());
      Serial.println(timeClient.getHours());
      return;

    } else timeClient.update();

    if (firstBoot) {
      LittleFS.begin();
      configTime(TZ_Europe_Berlin, "pool.ntp.org", "time.nist.gov");
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);
      int numCerts = certStore.initCertStore(LittleFS, PSTR("/certs.idx"), PSTR("/certs.ar"));
      BearSSL::WiFiClientSecure *bear = new BearSSL::WiFiClientSecure();
      bear->setCertStore(&certStore);

      client = new PubSubClient(*bear);
      client->setServer(mqtt_server, 8883);
      client->setCallback(callback);

      firstBoot = false;
      Serial.println("firstBoot");
    }
    if (!client->connected()) {
      String clientId = "hivemq.webclient.1674120443225";
      if (client->connect(clientId.c_str(), "CityFarm", "12345678")) {
        useMQTT = true;
        Serial.println("Connected to HiveMQ");
        client->subscribe("CityFarm/commands/light");
        client->subscribe("CityFarm/settings/watering_time");
      }
      else {
        Serial.println("failed to connect to HiveMQ");
        useMQTT = false;
      }
    } else client->loop();
  } else if (WiFi.status() == WL_DISCONNECTED) {
    Serial.println("Disconnected");
    WiFiConnected = false;
  } else if (WiFi.status() == WL_CONNECT_FAILED) {
    Serial.println("Connection failed");
    WiFi.begin(ssid, password);
  }
}


void setup() {
  Serial.begin(9600);
  //setup_wifi();
  WiFi.begin(ssid, password);
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(lamp, OUTPUT);
  pinMode(pomp, OUTPUT);

  OS.attach(0, watering, 10000);
  OS.attach(1, checkConnections, 500);
}

void loop() {
  OS.tick();
  now = time(nullptr);
  switch (pomp_status) {
    case 1:
      switch (WiFiConnected) {
        case 1:
          if (timeClient.getSeconds() >= watering_start_time + watering_time & timeClient.getMinutes() == watering_minutes) {
            digitalWrite(pomp, LOW);
            pomp_status = false;
            Serial.println("Watering ends");
          }
          break;
        case 0:
          if (rtc.getSeconds() >= watering_start_time + watering_time & rtc.getMinutes() == watering_minutes) {
            digitalWrite(pomp, LOW);
            pomp_status = false;
            Serial.println("Watering ends");
           }
          break;     
      }
    break;
  }}
