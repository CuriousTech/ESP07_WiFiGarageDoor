/**The MIT License (MIT)

Copyright (c) 2016 by Greg Cunningham, CuriousTech

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

// Build with Arduino IDE 1.8.9, esp8266 SDK 2.5.0

//uncomment to enable Arduino IDE Over The Air update code
#define OTA_ENABLE
#define USE_OLED
#define DEBUG

//#define USE_SPIFFS // Uses 7K more program space

#include <Wire.h>
#ifdef USE_OLED
#include <ssd1306_i2c.h> // https://github.com/CuriousTech/WiFi_Doorbell/tree/master/Libraries/ssd1306_i2c
#endif

#include <EEPROM.h>
#include <ESP8266mDNS.h>
#include <ESPAsyncWebServer.h> // https://github.com/me-no-dev/ESPAsyncWebServer
#include "RunningMedian.h"
#include <TimeLib.h> // http://www.pjrc.com/teensy/td_libs_Time.html
#include <UdpTime.h>
#include "PushBullet.h"
#include "eeMem.h"
#include <JsonParse.h> // https://github.com/CuriousTech/ESP8266-HVAC/tree/master/Libraries/JsonParse
#include <JsonClient.h>
#ifdef OTA_ENABLE
#include <FS.h>
#include <ArduinoOTA.h>
#endif
#ifdef USE_SPIFFS
#include <FS.h>
#include <SPIFFSEditor.h>
#else
#include "pages.h"
#endif
#include "jsonstring.h"
#include <AM2320.h>
#include <NewPingESP8266.h>

int serverPort = 80;                    // port fwd for fwdip.php

#define ESP_LED  2  // low turns on ESP blue LED
#define MOTION  12  //
#define SR1     13  //
#define SWITCH  14
#define SR2     16  //
#define REMOTE  15

enum reportReason
{
  Reason_Setup,
  Reason_Status,
  Reason_Alert,
  Reason_Motion,
};

IPAddress lastIP;
IPAddress verifiedIP;
int nWrongPass;

#ifdef USE_OLED
SSD1306 display(0x3c, 5, 4); // Initialize the oled display for address 0x3c, sda=5, sdc=4
#endif

const char hostName[] ="GDO";

AM2320 am;

AsyncWebServer server( serverPort );
AsyncWebSocket ws("/ws"); // access at ws://[esp ip]/ws

PushBullet pb;

void jsonCallback(int16_t iName, int iValue, char *psValue);
JsonParse jsonParse(jsonCallback);
void jsonPushCallback(int16_t iEvent, uint16_t iName, int iValue, char *psValue);
JsonClient jsonPush(jsonPushCallback);

UdpTime utime;

float temp;
float rh;

eeMem ee;

bool bDoorOpen;
bool bCarIn;
bool bPulseRemote;
bool bMotion;
uint16_t doorVal;
uint16_t carVal;
uint16_t doorOpenTimer;
uint16_t doorDelay;
uint16_t displayTimer;

bool bConfigDone = false;
bool bStarted = false;
uint32_t connectTimer;

#define SONAR_NUM    2   // Number of sensors.
#define MAX_DISTANCE 400 // Maximum distance (in cm) to ping.

NewPingESP8266 sonar[SONAR_NUM] = {   // Sensor object array.
  NewPingESP8266(SR1, SR1, MAX_DISTANCE), // GDO 
  NewPingESP8266(SR2, SR2, MAX_DISTANCE)  // Car
};

String sDec(int t) // just 123 to 12.3 string
{
  String s = String( t / 10 ) + ".";
  s += t % 10;
  return s;
}

String dataJson()
{
  jsonString js("state");

  js.Var("t", (uint32_t)now() - ( (ee.tz + utime.getDST() ) * 3600) );
  js.Var("door", bDoorOpen);
  js.Var("car", bCarIn);
  js.Var("temp", String(temp/10 + ((float)ee.tempCal/10), 1) );
  js.Var("rh", String(rh/10, 1) );
  js.Var("o", ee.bEnableOLED);
  js.Var("carVal", carVal);         // use value to check for what your threshold should be
  js.Var("doorVal", doorVal);
  js.Var("motion", bMotion);
  return js.Close();
}

String settingsJson()
{
  jsonString js("settings");

  js.Var("ct",  ee.nCarThresh);
  js.Var("dt",  ee.nDoorThresh);
  js.Var("tz",  ee.tz);
  js.Var("at",  ee.alarmTimeout);
  js.Var("clt",  ee.closeTimeout);
  js.Var("delay", ee.delayClose);
  js.Var("rt", ee.rate);
  String s = String(ee.hostIP[0]);
  s += ".";
  s += String(ee.hostIP[1]);
  s += ".";
  s += String(ee.hostIP[2]);
  s += ".";
  s += String(ee.hostIP[3]);
  js.Var("host", s);
  js.Var("rt", ee.rate);
  return js.Close();
}

void displayStart()
{
  if(ee.bEnableOLED == false && displayTimer == 0)
  {
#ifdef USE_OLED
    display.init();
//    display.flipScreenVertically();
#endif
  }
  displayTimer = 30;
}

void parseParams(AsyncWebServerRequest *request)
{
  char password[64];
 
  if(request->params() == 0)
    return;

//  Serial.println("parseParams");

  // get password first
  for ( uint8_t i = 0; i < request->params(); i++ ) {
    AsyncWebParameter* p = request->getParam(i);

    String s = request->urlDecode(p->value());

    switch( p->name().charAt(0)  )
    {
      case 'k': // key
        s.toCharArray(password, sizeof(password));
        break;
    }
  }

  IPAddress ip = request->client()->remoteIP();

  if( ip && ip == verifiedIP ); // can skip if last verified
  else if( strcmp(password, ee.szControlPassword) || nWrongPass)
  {
    if(nWrongPass == 0) // it takes at least 10 seconds to recognize a wrong password
      nWrongPass = 10;
    else if((nWrongPass & 0xFFFFF000) == 0 ) // time doubles for every high speed wrong password attempt.  Max 1 hour
      nWrongPass <<= 1;
    if(ip != lastIP)  // if different IP drop it down
       nWrongPass = 10;

    jsonString js("hack");
    js.Var("ip", ip.toString() );
    js.Var("pass", password);
    ws.textAll(js.Close());

    lastIP = ip;
    return;
  }

  verifiedIP = ip;
  lastIP = ip;

  const char Names[][11]={
    "closedelay", // 0
    "closeto",
    "alarmto",
    "cal",
    "door",
    "oled",
    "rate",
    "reset",
    "pbt",
    "hostip",
    "port",
    "",
  };

  for ( uint8_t i = 0; i < request->params(); i++ ) {
    AsyncWebParameter* p = request->getParam(i);
    String s = request->urlDecode(p->value());
    bool which = (tolower(p->name().charAt(1) ) == 'd') ? 1:0;
    int val = s.toInt();

    uint8_t idx;
    for(idx = 0; Names[idx][0]; idx++)
      if( p->name().equals(Names[idx]) )
        break;

    switch( idx )
    {
      case 0: // close/open delay
          ee.delayClose = val;
          break;
      case 1: // close timeout (set a bit higher than it takes to close)
          ee.closeTimeout = val;
          break;
      case 2: // alarm timeout
          ee.alarmTimeout = val;
          break;
      case 3: // temp offset
          ee.tempCal = val;
          break;
      case 4: // Door (pulse the output)
          displayStart();
          if(val) doorDelay = ee.delayClose;
          else bPulseRemote = true;
          break;
      case 5: // OLED
        ee.bEnableOLED = (s == "true") ? true:false;
        if(!ee.bEnableOLED)
          displayTimer = 0;
#ifdef USE_OLED
        display.clear();
        display.display();
#endif
        break;
      case 6: // WS/event update rate
        ee.rate = val;
        break;
      case 7: // reset
        ESP.reset();
        break;
      case 8: // pushbullet
        s.toCharArray( ee.pbToken, sizeof(ee.pbToken) );
        break;
      case 9:
        if(s.length() > 9)
        {
          ee.hostPort = 80;
          ip.fromString(s.c_str());
        }
        else
          ee.hostPort = val ? val:80;
        ee.hostIP[0] = ip[0];
        ee.hostIP[1] = ip[1];
        ee.hostIP[2] = ip[2];
        ee.hostIP[3] = ip[3];
        CallHost(Reason_Setup, ""); // test
        break;
      case 10:
        ee.hostPort = val ? val:80;
        break;
    }
  }
}

// Time in hh:mm[:ss][AM/PM]
String timeFmt(bool do_sec, bool do_M)
{
  String r = "";
  if(hourFormat12() < 10) r = " ";
  r += hourFormat12();
  r += ":";
  if(minute() < 10) r += "0";
  r += minute();
  if(do_sec)
  {
    r += ":";
    if(second() < 10) r += "0";
    r += second();
    r += " ";
  }
  if(do_M)
  {
      r += isPM() ? "PM":"AM";
  }
  return r;
}

const char *jsonList1[] = {
  "key",
  "doorDelay", // close/open delay
  "closetimeout", // close timeout
  "alarmtimeout",
  "threshDoor",
  "threshCar",
  "door",
  "tempOffset",
  "oled",
  "TZ",
  NULL
};

bool bKeyGood;
bool bDataMode;

void jsonCallback(int16_t iName, int iValue, char *psValue)
{
  if(bKeyGood == false && iName > 0)
    return;  // only allow key set

  switch(iName)
  {
    case 0: // key
      if(!strcmp(psValue, ee.szControlPassword)) // first item must be key
        bKeyGood = true;
      break;
    case 1: // doorDelay
      ee.delayClose = iValue;
      break;
    case 2: // closeTimeout
      ee.closeTimeout = iValue;
      break;
    case 3:
      ee.alarmTimeout = iValue;
      break;
    case 4: // threshDoor
      ee.nDoorThresh = iValue;
      break;
    case 5: // threshCar
      ee.nCarThresh = iValue;
      break;
    case 6: // Door
      displayStart();
      if(iValue) doorDelay = ee.delayClose;
      else bPulseRemote = true; // start output pulse
      break;
    case 7: // tempOffset
      ee.tempCal = iValue;
      break;
    case 8: // OLED
      ee.bEnableOLED = iValue ? true:false;
      break;
    case 9: // TZ
      ee.tz = iValue;
      break;
  }
}

const char *jsonListPush[] = { "",
  "time", // 0
  NULL
};

uint8_t failCnt;

void jsonPushCallback(int16_t iEvent, uint16_t iName, int iValue, char *psValue)
{
  switch(iEvent)
  {
    case -1: // status
      if(iName >= JC_TIMEOUT)
      {
        if(++failCnt > 5)
          ESP.restart();
      }
      else failCnt = 0;
      break;
    case 0: // time
      switch(iName)
      {
        case 0: // time
          setTime(iValue + ( (ee.tz + utime.getDST() ) * 3600));
          break;
      }
      break;
  }
}

void CallHost(reportReason r, String sStr)
{
  if(ee.hostIP[0] == 0 || WiFi.status() != WL_CONNECTED) // no host set
    return;

  String sUri = String("/wifi?name=\"GDO\"&reason=");

  switch(r)
  {
    case Reason_Setup:
      sUri += "setup&port="; sUri += serverPort;
      break;
    case Reason_Status:
      sUri += "status&door="; sUri += bDoorOpen;
      sUri += "&car="; sUri += bCarIn;
      sUri += "&temp="; sUri += String(temp/10,1);
      sUri += "&rh="; sUri += String(rh/10,1);
      break;
    case Reason_Alert:
      sUri += "alert&value=\"";
      sUri += sStr;
      sUri += "\"";
      break;
    case Reason_Motion:
      sUri += "motion";
      break;
  }

  IPAddress ip(ee.hostIP);
  String url = ip.toString();
  jsonPush.begin(url.c_str(), sUri.c_str(), ee.hostPort, false, false, NULL, NULL);
  jsonPush.addList(jsonListPush);
}

void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len)
{  //Handle WebSocket event
  static bool bRestarted = true;
  String s;

  switch(type)
  {
    case WS_EVT_CONNECT:      //client connected
      if(bRestarted)
      {
        bRestarted = false;
        client->text( "{\"cmd\":\"alert\",\"text\":\"Restarted\"}" );
      }
      client->keepAlivePeriod(50);
      client->text( dataJson() );
      client->text( settingsJson() );
      client->ping();
      break;
    case WS_EVT_DISCONNECT:    //client disconnected
      break;
    case WS_EVT_ERROR:    //error was received from the other end
      break;
    case WS_EVT_PONG:    //pong message was received (in response to a ping request maybe)
      break;
    case WS_EVT_DATA:  //data packet
      AwsFrameInfo * info = (AwsFrameInfo*)arg;
      if(info->final && info->index == 0 && info->len == len){
        //the whole message is in a single frame and we got all of it's data
        if(info->opcode == WS_TEXT){
          data[len] = 0;

          uint32_t ip = client->remoteIP();

          bKeyGood = (ip && verifiedIP == ip) ? true:false; // if this IP sent a good key, no need for more
          jsonParse.process((char*)data);
          if(bKeyGood)
            verifiedIP = ip;
        }
      }
      break;
  }
}

void setup()
{
  pinMode(MOTION, INPUT);
  pinMode(ESP_LED, OUTPUT);
  pinMode(REMOTE, OUTPUT);
  pinMode(SWITCH, OUTPUT);
  digitalWrite(ESP_LED, LOW);
  digitalWrite(REMOTE, LOW);
  digitalWrite(SWITCH, HIGH);

  // initialize dispaly
#ifdef USE_OLED
  display.init();
//  display.flipScreenVertically();
  display.clear();
  display.display();
#else
  am.begin(5, 4);
#endif

#ifdef DEBUG
  Serial.begin(115200);
//  delay(3000);
  Serial.println();
  Serial.println();
#endif

  WiFi.hostname(hostName);
  WiFi.mode(WIFI_STA);

  if ( ee.szSSID[0] )
  {
    WiFi.begin(ee.szSSID, ee.szSSIDPassword);
    WiFi.setHostname(hostName);
    bConfigDone = true;
  }
  else
  {
    Serial.println("No SSID. Waiting for EspTouch.");
    WiFi.beginSmartConfig();
  }
  connectTimer = now();

#ifdef USE_SPIFFS
  SPIFFS.begin();
  server.addHandler(new SPIFFSEditor("admin", controlPassword));
#endif

  // attach AsyncWebSocket
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.on( "/", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request){
    bDataMode = false; // turn off numeric display and frequent updates
  });
  server.on ( "/iot", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request )
  {
    bDataMode = false; // turn off numeric display and frequent updates
    parseParams(request);
#ifdef USE_SPIFFS
    request->send(SPIFFS, "/index.html");
#else
    request->send_P(200, "text/html", page1);
#endif
  });
  server.on( "/setup.html", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request){
    parseParams(request);
    bDataMode = true;
#ifdef USE_SPIFFS
    request->send(SPIFFS, "/setup.html");
#else
    request->send_P(200, "text/html", page2);
#endif
  });
  server.on( "/s", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request){
    parseParams(request);

    String page = "{\"ip\": \"";
    page += WiFi.localIP().toString();
    page += ":";
    page += serverPort;
    page += "\"}";
    request->send( 200, "text/json", page );
  });
  server.on( "/json", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request){
    parseParams(request);
    request->send( 200, "text/json", settingsJson() );
  });
  server.on("/heap", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", String(ESP.getFreeHeap()));
  });
  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request){
    AsyncWebServerResponse *response = request->beginResponse_P(200, "image/x-icon", favicon, sizeof(favicon));
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
  });

  server.onNotFound([](AsyncWebServerRequest *request){
//    request->send(404);
  });

  server.onFileUpload([](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
  });
  server.onRequestBody([](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
  });

  server.begin();

#ifdef OTA_ENABLE
  ArduinoOTA.setHostname(hostName);
  ArduinoOTA.begin();
#endif

  jsonParse.setList(jsonList1);
  digitalWrite(ESP_LED, HIGH);
  if(ee.rate == 0) ee.rate = 60;
}

uint16_t stateTimer = ee.rate;

void sendState()
{
  ws.textAll(dataJson());
  stateTimer = ee.rate;
}

RunningMedian<uint16_t, 12> rangeMedian[2];
RunningMedian<uint16_t, 20> tempMedian[2];

void loop()
{
  static uint8_t hour_save, sec_save;
  static uint8_t cnt = 0;
  static uint16_t oldCarVal, oldDoorVal;
  bool bNew;
  static bool bReleaseRemote;
  static bool bClear;

  MDNS.update();
#ifdef OTA_ENABLE
  ArduinoOTA.handle();
#endif
  
  if(WiFi.status() == WL_CONNECTED && ee.useTime)
    utime.check(ee.tz);

  if(bDataMode && (carVal != oldCarVal || doorVal != oldDoorVal) ) // high speed update
  {
    oldCarVal = carVal;
    oldDoorVal = doorVal;
    ws.textAll( dataJson() );
  }

  if(digitalRead(MOTION) != bMotion)
  {
    bMotion = digitalRead(MOTION);
    if(bMotion)
    {
      displayStart();
      sendState();
      CallHost(Reason_Motion,"");
    }
  }

  if(sec_save != second()) // only do stuff once per second (loop is maybe 20-30 Hz)
  {
    sec_save = second();

    if(!bConfigDone)
    {
      if( WiFi.smartConfigDone())
      {
        Serial.println("SmartConfig set");
        bConfigDone = true;
        connectTimer = now();
      }
    }
    if(bConfigDone)
    {
      if(WiFi.status() == WL_CONNECTED)
      {
        if(!bStarted)
        {
          Serial.println("WiFi Connected");
          MDNS.begin( hostName );
          bStarted = true;

          CallHost(Reason_Setup, "");
          if(ee.useTime)
            utime.start();

          MDNS.addService("iot", "tcp", serverPort);
          WiFi.SSID().toCharArray(ee.szSSID, sizeof(ee.szSSID)); // Get the SSID from SmartConfig or last used
          WiFi.psk().toCharArray(ee.szSSIDPassword, sizeof(ee.szSSIDPassword) );
        }
      }
      else if(now() - connectTimer > 10) // failed to connect for some reason
      {
        Serial.println("Connect failed. Starting SmartConfig");
        connectTimer = now();
        ee.szSSID[0] = 0;
        WiFi.mode(WIFI_AP_STA);
        WiFi.beginSmartConfig();
        bConfigDone = false;
        bStarted = false;
      }
    }

    float av;
    rangeMedian[1].getAverage(2, av);
    doorVal = av;

    bNew = (doorVal < ee.nDoorThresh) ? true:false;
    if(bNew != bDoorOpen)
    {
      displayStart();
      bDoorOpen = bNew;
      doorOpenTimer = bDoorOpen ? ee.alarmTimeout : 0;
      sendState();
      CallHost(Reason_Status,"");
    }

    rangeMedian[0].getAverage(2, av);
    carVal = av;
    bNew = (carVal < ee.nCarThresh) ? true:false; // lower is closer

    if(carVal < 15) // something is < 25cm away
    {
      displayStart();
    }

    if(bNew != bCarIn)
    {
      bCarIn = bNew;
      if(bCarIn)
      {
        displayStart();
      }
      sendState();
      CallHost(Reason_Status,"");
    }

    if (hour_save != hour())
    {
      hour_save = hour();
      if((hour_save&1) == 0)
        CallHost(Reason_Setup,"");
      CallHost(Reason_Status,"");
      if(hour_save == 2 && ee.useTime)
      {
        utime.start(); // update time daily at DST change
      }
      ee.update(); // update EEPROM if needed while we're at it (give user time to make many adjustments)
    }

    if(second() == 58) // 2 seconds before
      digitalWrite(SWITCH, LOW);
    else if(second() == 0)
    {
      float temp2, rh2;
      if(am.measure(temp2, rh2))
      {
        digitalWrite(SWITCH, HIGH);
        tempMedian[0].add( (1.8 * temp2 + 32.0) * 10 );
        tempMedian[0].getAverage(2, temp2);
        tempMedian[1].add(rh2 * 10);
        tempMedian[1].getAverage(2, rh2);
        if(temp != temp2)
        {
          temp = temp2;
          rh = rh2;
          sendState();
          CallHost(Reason_Status,"");
        }
      }
      display.init();
    }

    if(nWrongPass)
      nWrongPass--;

    if(bReleaseRemote) // reset remote output 1 second after it started
    {
      digitalWrite(REMOTE, LOW);
      bReleaseRemote = false;
    }
    if(bPulseRemote)
    {
      if(bDoorOpen == false) // closing door
      {
         doorOpenTimer = ee.closeTimeout; // set the short timeout
      }
//      Serial.println("pulseRemote");
      digitalWrite(REMOTE, HIGH);
      bPulseRemote = false;
      bReleaseRemote = true;
    }

    if(doorDelay) // delayed close/open
    {
      if(--doorDelay == 0)
        bPulseRemote = true;
    }

    if(doorOpenTimer && doorDelay == 0) // door open watchdog
    {
      if(--doorOpenTimer == 0)
      {
        CallHost(Reason_Alert, "Door not closed");
        ws.textAll( "{\"cmd\":\"alert\",\"text\":\"Door not closed\"}" );
        pb.send("GDO", "Door not closed", ee.pbToken);
      }
    }

    if(--stateTimer == 0) // a 60 second keepAlive
      sendState();

    if(displayTimer) // temp display on thing
      displayTimer--;

    uint8_t x = (second() & 3);
  
    // draw the screen here
    display.clear();
    if(ee.bEnableOLED || displayTimer)
    {
      display.drawPropString(x, 0, timeFmt(true, true) );
  
      if(bDataMode) // display numbers when the setup page is loaded
      {
        display.drawPropString(x+ 2, 23, String(doorVal) );
        display.drawPropString(x+80, 23, String(carVal) ); // cm
      }
      else  // normal status
      {
        display.drawPropString(x+ 2, 23, bDoorOpen ? "Open":"Closed" );
        display.drawPropString(x+80, 23, bCarIn ? "In":"Out" );
      }
      display.drawPropString(x+2, 47, String(temp/10 + ((float)ee.tempCal/10), 1) + "]");
      display.drawPropString(x+64, 47, String(rh/10, 1) + "%");
    }
    display.display();
  }

  static uint32_t rangeTime;
  static uint8_t bTog;
  if(millis() - rangeTime >= 100) {
    rangeTime = millis();
    uint16_t ul = sonar[bTog].ping_cm();
    rangeMedian[bTog].add( ul );
    bTog = bTog ? 0:1;
  }
}
