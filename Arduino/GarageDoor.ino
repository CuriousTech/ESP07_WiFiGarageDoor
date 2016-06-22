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

// Build with Arduino IDE 1.6.8, esp8266 SDK 2.2.0

#include <Wire.h>
#include <DHT.h> // http://www.github.com/markruys/arduino-DHT
#include <TimeLib.h> // http://www.pjrc.com/teensy/td_libs_Time.html
#include <ssd1306_i2c.h> // https://github.com/CuriousTech/WiFi_Doorbell/tree/master/Libraries/ssd1306_i2c

#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <EEPROM.h>
#include <ESP8266mDNS.h>
#include "WiFiManager.h"
#include <ESP8266WebServer.h>
#include <Event.h>  // https://github.com/CuriousTech/ESP8266-HVAC/tree/master/Libraries/Event
 
const char controlPassword[] = "password"; // device password for modifying any settings
int serverPort = 84;                    // port fwd for fwdip.php

#define ESP_LED    2  // low turns on ESP blue LED
#define ECHO      12  // the voltage divider to the bottom corner pin (short R7, don't use R8)
#define TRIG      13  // the direct pin (bottom 3 pin header)
#define DHT_22    14
#define REMOTE    15

uint32_t lastIP;
int nWrongPass;

SSD1306 display(0x3c, 5, 4); // Initialize the oled display for address 0x3c, sda=5, sdc=4

WiFiManager wifi(0);  // AP page:  192.168.4.1
ESP8266WebServer server( serverPort );
const int NTP_PACKET_SIZE = 48; // NTP time stamp is in the first 48 bytes of the message
byte packetBuffer[ NTP_PACKET_SIZE]; //buffer to hold incoming and outgoing packets
WiFiUDP Udp;
bool bNeedUpdate;
uint8_t  dst;           // current dst

DHT dht;
float temp;
float rh;
int httpPort = 80; // may be modified by open AP scan

struct eeSet // EEPROM backed data
{
  uint16_t size;          // if size changes, use defauls
  uint16_t sum;           // if sum is diiferent from memory struct, write
  int8_t   tz;            // Timezone offset
  uint8_t  dst;
  uint16_t nCarThresh;
  uint16_t nDoorThresh;
  uint16_t alarmTimeout;
  uint16_t closeTimeout;
  int8_t   tempCal;
  bool     bEnableOLED;
  char     pbToken[40];
};
eeSet ee = { sizeof(eeSet), 0xAAAA,
  -5, 0,     // TZ, dst
  500, 900,  // thresholds
  5*60,      // alarmTimeout
  60,        // closeTimeout (seconds)
  0,         // adjust for error
  true,    // OLED
  "pushbullet token" // pushbullet token
};

int ee_sum; // sum for checking if any setting has changed
bool bDoorOpen;
bool bCarIn;
uint16_t doorVal;
uint16_t carVal;
uint16_t doorOpenTimer;

String dataJson()
{
    String s = "{\"door\": ";
    s += bDoorOpen;
    s += ",\"car\": ";
    s += bCarIn;
    s += ",\"temp\": \"";
    s += String(adjTemp(), 1);
    s += "\",\"rh\": \"";
    s += String(adjRh(), 1);
    s += "\"}";
    return s;
}

eventHandler event(dataJson);

String ipString(IPAddress ip) // Convert IP to string
{
  String sip = String(ip[0]);
  sip += ".";
  sip += ip[1];
  sip += ".";
  sip += ip[2];
  sip += ".";
  sip += ip[3];
  return sip;
}

bool parseArgs()
{
  char temp[100];
  String password;
  bool bRemote = false;
  bool ipSet = false;

  Serial.println("parseArgs");

  // get password first
  for ( uint8_t i = 0; i < server.args(); i++ ) {
    server.arg(i).toCharArray(temp, 100);
    String s = wifi.urldecode(temp);
    switch( server.argName(i).charAt(0)  )
    {
      case 'k': // key
          password = s;
          break;
    }
  }

  if(password == controlPassword)
  for ( uint8_t i = 0; i < server.args(); i++ ) {
    server.arg(i).toCharArray(temp, 100);
    String s = wifi.urldecode(temp);
//    Serial.println( i + " " + server.argName ( i ) + ": " + s);
    bool which = (tolower(server.argName(i).charAt(1) ) == 'D') ? 1:0;
    int val = s.toInt();
 
    switch( server.argName(i).charAt(0)  )
    {
      case 'Z': // TZ
          ee.tz = val;
          getUdpTime();
          break;
      case 'C': // close timeout (set a bit higher than it takes to close)
          ee.closeTimeout = val;
          break;
      case 'T': // alarm timeout
          ee.alarmTimeout = val;
          break;
      case 'L':  // Thresholds, Ex set car=290, door=500 : hTtp://192.168.0.190:84?LC=290&LD=500&key=password
           if(which) // LD
           {
             ee.nDoorThresh = val;
           }
           else // LC
           {
             ee.nCarThresh = val;
           }
           break;
      case 'F': // temp offset
          ee.tempCal = val;
          break;
      case 'D': // Door (pulse the output)
          bRemote = true;
          break;
      case 'O': // OLED
          ee.bEnableOLED = (s == "true") ? true:false;
          break;
      case 'p': // pushbullet
          s.toCharArray( ee.pbToken, sizeof(ee.pbToken) );
          break;
    }
  }

  uint32_t ip = server.client().remoteIP();

  if(server.args() && (password != controlPassword) )
  {
    if(nWrongPass == 0) // it takes at least 10 seconds to recognize a wrong password
      nWrongPass = 10;
    else if((nWrongPass & 0xFFFFF000) == 0 ) // time doubles for every high speed wrong password attempt.  Max 1 hour
      nWrongPass <<= 1;
    if(ip != lastIP)  // if different IP drop it down
       nWrongPass = 10;
    String data = "{\"ip\":\"";
    data += ipString(ip);
    data += "\",\"pass\":\"";
    data += password;
    data += "\"}";
    event.push("hack", data); // log attempts
    bRemote = false;
  }

  lastIP = ip;
  if(bRemote)
     pulseRemote();
}

void handleRoot() // Main webpage interface
{
  Serial.println("handleRoot");

  parseArgs();

    String page = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"/>"
      "<title>WiFi Garage Door Opener</title>"
      "<style type=\"text/css\">\n"
      "div,table,input{\n"
      "border-radius: 5px;\n"
      "margin-bottom: 5px;\n"
      "box-shadow: 2px 2px 12px #000000;\n"
      "background-image: -moz-linear-gradient(top, #ffffff, #50a0ff);\n"
      "background-image: -ms-linear-gradient(top, #ffffff, #50a0ff);\n"
      "background-image: -o-linear-gradient(top, #ffffff, #50a0ff);\n"
      "background-image: -webkit-linear-gradient(top, #efffff, #50a0ff);\n"
      "background-image: linear-gradient(top, #ffffff, #50a0ff);\n"
      "background-clip: padding-box;\n"
      "}\n"
      "body{width:240px;font-family: Arial, Helvetica, sans-serif;}\n"
      ".style5 {\n"
      "border-radius: 5px;\n"
      "box-shadow: 0px 0px 15px #000000;\n"
      "background-image: -moz-linear-gradient(top, #ff00ff, #ffa0ff);\n"
      "background-image: -ms-linear-gradient(top, #ff00ff, #ffa0ff);\n"
      "background-image: -o-linear-gradient(top, #ff00ff, #ffa0ff);\n"
      "background-image: -webkit-linear-gradient(top, #ff0000, #ffa0a0);\n"
      "background-image: linear-gradient(top, #ff00ff, #ffa0ff);\n"
      "}\n"
      "</style>"

    "<script src=\"http://ajax.googleapis.com/ajax/libs/jquery/1.3.2/jquery.min.js\" type=\"text/javascript\" charset=\"utf-8\"></script>\n"
    "<script type=\"text/javascript\">\n"
    "a=document.all;"
    "oledon=";
  page += ee.bEnableOLED;
  page += ";"
    "function startEvents()"
    "{"
      "eventSource = new EventSource(\"events?i=60&p=1\");"
      "eventSource.addEventListener('open', function(e){},false);"
      "eventSource.addEventListener('error', function(e){},false);"
      "eventSource.addEventListener('alert', function(e){alert(e.data)},false);"
      "eventSource.addEventListener('state',function(e){"
        "d = JSON.parse(e.data);"
        "a.car.innerHTML = d.car?\"IN\":\"OUT\";"
        "a.car.setAttribute('class',d.car?'':'style5');"
        "a.door.innerHTML = d.door?\"OPEN\":\"CLOSED\";"
        "a.door.setAttribute('class',d.door?'style5':'');"
        "a.doorBtn.value = d.door?\"Close\":\"Open\";"
        "a.temp.innerHTML = d.temp+\"&degF\";"
        "a.rh.innerHTML = d.rh+'%';"
      "},false)"
    "}"
    "function setDoor(){"
    "$.post(\"s\", { D: 0, key: document.all.myKey.value })"
    "}"
    "function oled(){"
      "oledon=!oledon;"
      "$.post(\"s\", { O: oledon, key: document.all.myKey.value });"
      "document.all.OLED.value=oledon?'OFF':'ON'"
    "}"
    "setInterval(timer,1000);"
    "t=";
    page += now() - ((ee.tz + dst) * 3600); // set to GMT
    page +="000;function timer(){" // add 000 for ms
          "t+=1000;d=new Date(t);"
          "document.all.time.innerHTML=d.toLocaleTimeString()}"
      "</script>"

      "<body onload=\"{"
      "key = localStorage.getItem('key'); if(key!=null) document.getElementById('myKey').value = key;"
      "for(i=0;i<document.forms.length;i++) document.forms[i].elements['key'].value = key;"
      "startEvents();}\">";

    page += "<div><h3>WiFi Garage Door Opener </h3>"
            "<table align=\"center\">"
            "<tr align=\"center\"><td><p id=\"time\">";
    page += timeFmt(true, true);
    page += "</p></td><td>";
    page += "<input type=\"button\" value=\"Open\" id=\"doorBtn\" onClick=\"{setDoor()}\">";
    page += "</td></tr>"
          "<tr align=\"center\"><td>Garage</td>"
          "<td>Car</td>"
          "</tr><tr align=\"center\">"
          "<td>"
          "<div id=\"door\"";
    if(bDoorOpen)
      page += " class='style5'";
    page += ">";
    page += bDoorOpen ? "OPEN" : "CLOSED";
    page += "</div></td>"
            "<td>"
            "<div id=\"car\"";
    if(bCarIn == false)
      page += " class='style5'";
    page += ">";
    page += bCarIn ? "IN" : "OUT";
    page += "</div></td></tr>"
            "<tr align=\"center\"><td>" // temp/rh row
            "<div id=\"temp\">";
    page +=  String(adjTemp(), 1);
    page += "&degF</div></td><td><div id=\"rh\">";
    page += String(rh, 1);
    page += "%</div></td></tr><tr align=\"center\"><td>Timeout</td><td>Timezone</td></tr>"
            "<tr><td>";
    page += valButton("C", String(ee.closeTimeout) );
    page += "</td><td>";  page += valButton("Z", String(ee.tz) );
    page += "</td></tr>"
            "<tr><td>Display:</td><td>"
            "<input type=\"button\" value=\"";
    page += ee.bEnableOLED ? "OFF":"ON";
    page += "\" id=\"OLED\" onClick=\"{oled()}\">"
            "</td></tr>"

            "</table>"
            "<input id=\"myKey\" name=\"key\" type=text size=50 placeholder=\"password\" style=\"width: 150px\">"
            "<input type=\"button\" value=\"Save\" onClick=\"{localStorage.setItem('key', key = document.all.myKey.value)}\">"
            "<br><small>Logged IP: ";
    page += ipString(server.client().remoteIP());
    page += "</small></div></body></html>";

    server.send ( 200, "text/html", page );
}

float adjTemp()
{
  int t = (temp * 10) + ee.tempCal;
  return ((float)t / 10);
}

float adjRh()
{
  int t = (rh * 10);
  return ((float)t / 10);
}

String button(String id, String text) // Up/down buttons
{
  String s = "<form method='post'><input name='";
  s += id;
  s += "' type='submit' value='";
  s += text;
  s += "'><input type=\"hidden\" name=\"key\" value=\"value\"></form>";
  return s;
}

String sDec(int t)
{
  String s = String( t / 10 ) + ".";
  s += t % 10;
  return s;
}

String valButton(String id, String val)
{
  String s = "<form method='post'><input name='";
  s += id;
  s += "' type=text size=4 value='";
  s += val;
  s += "'><input type=\"hidden\" name=\"key\"><input value=\"Set\" type=submit></form>";
  return s;
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

void handleS()
{
    parseArgs();

    String page = "{\"ip\": \"";
    page += ipString(WiFi.localIP());
    page += ":";
    page += serverPort;
    page += "\"}";
    server.send ( 200, "text/json", page );
}

// JSON format for initial or full data read
void handleJson()
{
  String page = "{\"carThresh\": ";
  page += ee.nCarThresh;
  page += ", \"doorThresh\": ";
  page += ee.nDoorThresh;
  page += ", \"carVal\": ";
  page += carVal;         // use value to check for what your threshold should be
  page += ", \"doorVal\": ";
  page += doorVal;
  page += ", \"doorOpen\": ";
  page += bDoorOpen;
  page += ", \"carIn\": ";
  page += bCarIn;
  page += ", \"alarmTimeout\": ";
  page += ee.alarmTimeout;
  page += ", \"closeTimeout\": ";
  page += ee.closeTimeout;
  page += ",\"temp\": \"";
  page += String(adjTemp(), 1);
  page += "\",\"rh\": \"";
  page += String(adjRh(), 1);
  page += "\"}";

  server.send( 200, "text/json", page );
}

// event streamer (assume keep-alive) (esp8266 2.1.0 can't handle this)
void handleEvents()
{
  char temp[100];
  Serial.println("handleEvents");
  uint16_t interval = 60; // default interval
  uint8_t nType = 0;

  for ( uint8_t i = 0; i < server.args(); i++ ) {
    server.arg(i).toCharArray(temp, 100);
    String s = wifi.urldecode(temp);
//    Serial.println( i + " " + server.argName ( i ) + ": " + s);
    int val = s.toInt();
 
    switch( server.argName(i).charAt(0)  )
    {
      case 'i': // interval
        interval = val;
        break;
      case 'p': // push
        nType = 1;
        break;
      case 'c': // critical
        nType = 2;
        break;
    }
  }

  String content = "HTTP/1.1 200 OK\r\n"
      "Connection: keep-alive\r\n"
      "Access-Control-Allow-Origin: *\r\n"
      "Content-Type: text/event-stream\r\n\r\n";
  server.sendContent(content);
  event.set(server.client(), interval, nType); // copying the client before the send makes it work with SDK 2.2.0
}

void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += ( server.method() == HTTP_GET ) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";

  for ( uint8_t i = 0; i < server.args(); i++ ) {
    message += " " + server.argName ( i ) + ": " + server.arg ( i ) + "\n";
  }

  server.send ( 404, "text/plain", message );
}


void setup()
{
  pinMode(ESP_LED, OUTPUT);
  pinMode(TRIG, OUTPUT);      // HC-SR04 trigger
  pinMode(ECHO, INPUT);
  pinMode(REMOTE, OUTPUT);
  digitalWrite(REMOTE, LOW);

  digitalWrite(TRIG, LOW);

  // initialize dispaly
  display.init();

  display.clear();
  display.display();

  Serial.begin(115200);
//  delay(3000);
  Serial.println();
  Serial.println();

  WiFi.hostname("gdo");
  wifi.autoConnect("GDO");
  eeRead(); // don't access EE before WiFi init

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  if ( MDNS.begin ( "gdo", WiFi.localIP() ) ) {
    Serial.println ( "MDNS responder started" );
  }

  server.on ( "/", handleRoot );
  server.on ( "/s", handleS );
  server.on ( "/json", handleJson );
  server.on ( "/events", handleEvents );
//  server.on ( "/inline", []() {
//    server.send ( 200, "text/plain", "this works as well" );
//  } );
  server.onNotFound ( handleNotFound );
  server.begin();
  MDNS.addService("http", "tcp", serverPort);

  dht.setup(DHT_22, DHT::DHT22);
  getUdpTime();
}

void loop()
{
  static uint8_t hour_save, sec_save;
  static uint8_t tog = 0;
  static uint8_t cnt = 0;
#define ANA_AVG 24
  static uint16_t vals[2][ANA_AVG];
  static uint8_t ind[2];
  bool bNew;

  MDNS.update();
  server.handleClient();

  if(bNeedUpdate)
    checkUdpTime();

  if(sec_save != second()) // only do stuff once per second (loop is maybe 20-30 Hz)
  {
    sec_save = second();

    doorVal = 0;
    for(int i = 0; i < ANA_AVG; i++)
      doorVal += vals[0][i];
    doorVal /= ANA_AVG;
    bNew = (doorVal > ee.nDoorThresh) ? true:false;
    if(bNew != bDoorOpen)
    {
        bDoorOpen = bNew;
        doorOpenTimer = bDoorOpen ? ee.alarmTimeout : 0;
        event.push();
    }

    carVal = 0;
    for(int i = 0; i < ANA_AVG; i++)
      carVal += vals[1][i];
    carVal /= ANA_AVG;

    bNew = (carVal < ee.nCarThresh) ? true:false; // lower is closer

    if(bNew != bCarIn)
    {
      bCarIn = bNew;
      event.push();
    }

    if (hour_save != hour()) // update our IP and time daily (at 2AM for DST)
    {
      hour_save = hour();
      if(hour_save == 2)
      {
        getUdpTime();
      }
      eeWrite(); // update EEPROM if needed while we're at it (give user time to make many adjustments)
    }

    static uint8_t dht_cnt = 5;
    if(--dht_cnt == 0)
    {
      dht_cnt = 5;
      float newtemp = dht.toFahrenheit(dht.getTemperature());
      float newrh = dht.getHumidity();
      if(dht.getStatus() == DHT::ERROR_NONE && (temp != newtemp || rh != newrh))
      {
        temp = newtemp;
        rh = newrh;
        event.pushInstant();
      }
    }
    if(nWrongPass)
      nWrongPass--;

    event.heartbeat();

    if(doorOpenTimer)
    {
      if(--doorOpenTimer == 0)
      {
        event.alert("Door not closed");
        pushBullet("GDO", "Door not closed");
      }
    }

    unsigned long range;

    digitalWrite(TRIG, HIGH); // pulse the rangefinder
    delayMicroseconds(10);
    digitalWrite(TRIG, LOW);
    range = ( (pulseIn(ECHO, HIGH) / 2) / 29.1 );
    vals[1][ind[1]] = range; // read current IR sensor value into current circle buf
    if(++ind[1] >= ANA_AVG) ind[1] = 0;
    
    digitalWrite(ESP_LED, LOW);
    delay(20);
    digitalWrite(ESP_LED, HIGH);
  }

  DrawScreen();

  vals[tog][ind[tog]] = analogRead(A0); // read current IR sensor value into current circle buf
  if(++ind[tog] >= ANA_AVG) ind[tog] = 0;
}

void DrawScreen()
{
  static int16_t ind;
  // draw the screen here
  display.clear();

  const char days[7][4] = {"Sun","Mon","Tue","Wed","Thr","Fri","Sat"};
  const char months[12][4] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};

  if(ee.bEnableOLED)
  {
    String s = timeFmt(true, true);
    s += "  ";
    s += days[weekday()-1];
    s += " ";
    s += String(day());
    s += " ";
    s += months[month()-1];
    s += "  ";
    int len = s.length();
    s = s + s;

    int w = display.drawPropString(ind, 0, s );
    if( --ind < -(w)) ind = 0;

    display.drawPropString( 2, 23, bDoorOpen ? "Open":"Closed" );
    display.drawPropString(80, 23, bCarIn ? "In":"Out" );

    display.drawPropString(2, 47, String(adjTemp(), 1) + "]");
    display.drawPropString(64, 47, String(adjRh(), 1) + "%");

  }
  display.display();
}

void pulseRemote()
{
  if(bDoorOpen == false) // closing door
  {
    doorOpenTimer = ee.closeTimeout; // set the short timeout 
  }
  Serial.println("pulseRemote");
  digitalWrite(REMOTE, HIGH);
  delay(1000);
  digitalWrite(REMOTE, LOW);
}

void eeWrite() // write the settings if changed
{
  uint16_t old_sum = ee.sum;
  ee.sum = 0;
  ee.sum = Fletcher16((uint8_t *)&ee, sizeof(eeSet));

  if(old_sum == ee.sum)
    return; // Nothing has changed?

  wifi.eeWriteData(64, (uint8_t*)&ee, sizeof(ee)); // WiFiManager already has an instance open, so use that at offset 64+
}

void eeRead()
{
  eeSet eeTest;

  wifi.eeReadData(64, (uint8_t*)&eeTest, sizeof(eeSet));
  if(eeTest.size != sizeof(eeSet)) return; // revert to defaults if struct size changes
  uint16_t sum = eeTest.sum;
  eeTest.sum = 0;
  eeTest.sum = Fletcher16((uint8_t *)&eeTest, sizeof(eeSet));
  if(eeTest.sum != sum) return; // revert to defaults if sum fails
  memcpy(&ee, &eeTest, sizeof(eeSet));
}

uint16_t Fletcher16( uint8_t* data, int count)
{
   uint16_t sum1 = 0;
   uint16_t sum2 = 0;

   for( int index = 0; index < count; ++index )
   {
      sum1 = (sum1 + data[index]) % 255;
      sum2 = (sum2 + sum1) % 255;
   }

   return (sum2 << 8) | sum1;
}

void getUdpTime()
{
  if(bNeedUpdate) return;
//  Serial.println("getUdpTime");
  Udp.begin(2390);
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;     // Stratum, or type of clock
  packetBuffer[2] = 6;     // Polling Interval
  packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12]  = 49;
  packetBuffer[13]  = 0x4E;
  packetBuffer[14]  = 49;
  packetBuffer[15]  = 52;
  
  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  // time.nist.gov
  Udp.beginPacket("0.us.pool.ntp.org", 123); //NTP requests are to port 123
  Udp.write(packetBuffer, NTP_PACKET_SIZE);
  Udp.endPacket();
  bNeedUpdate = true;
}

void DST() // 2016 starts 2AM Mar 13, ends Nov 6
{
  tmElements_t tm;
  breakTime(now(), tm);
  // save current time
  uint8_t m = tm.Month;
  int8_t d = tm.Day;
  int8_t dow = tm.Wday;

  tm.Month = 3; // set month = Mar
  tm.Day = 14; // day of month = 14
  breakTime(makeTime(tm), tm); // convert to get weekday

  uint8_t day_of_mar = (7 - tm.Wday) + 8; // DST = 2nd Sunday

  tm.Month = 11; // set month = Nov (0-11)
  tm.Day = 7; // day of month = 7 (1-30)
  breakTime(makeTime(tm), tm); // convert to get weekday

  uint8_t day_of_nov = (7 - tm.Wday) + 1;

  if ((m  >  3 && m < 11 ) ||
      (m ==  3 && d > day_of_mar) ||
      (m ==  3 && d == day_of_mar && hour() >= 2) ||  // DST starts 2nd Sunday of March;  2am
      (m == 11 && d <  day_of_nov) ||
      (m == 11 && d == day_of_nov && hour() < 2))   // DST ends 1st Sunday of November; 2am
   dst = 1;
 else
   dst = 0;
}

bool checkUdpTime()
{
  static int retry = 0;

  if(!Udp.parsePacket())
  {
    if(++retry > 500)
     {
        getUdpTime();
        retry = 0;
     }
    return false;
  }
//  Serial.println("checkUdpTime good");

  // We've received a packet, read the data from it
  Udp.read(packetBuffer, NTP_PACKET_SIZE); // read the packet into the buffer

  Udp.stop();
  // the timestamp starts at byte 40 of the received packet and is four bytes,
  // or two words, long. First, extract the two words:

  unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
  unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
  unsigned long secsSince1900 = highWord << 16 | lowWord;
  // Unix time starts on Jan 1 1970. In seconds, that's 2208988800:
  const unsigned long seventyYears = 2208988800UL;
  long timeZoneOffset = 3600 * (ee.tz + dst);
  unsigned long epoch = secsSince1900 - seventyYears + timeZoneOffset + 1; // bump 1 second

  // Grab the fraction
  highWord = word(packetBuffer[44], packetBuffer[45]);
  lowWord = word(packetBuffer[46], packetBuffer[47]);
  unsigned long d = (highWord << 16 | lowWord) / 4295000; // convert to ms
  delay(d); // delay to next second (meh)
  setTime(epoch);
  DST(); // check the DST and reset clock
  timeZoneOffset = 3600 * (ee.tz + dst);
  epoch = secsSince1900 - seventyYears + timeZoneOffset + 1; // bump 1 second
  setTime(epoch);

//  Serial.print("Time ");
//  Serial.println(timeFmt(true, true));
  bNeedUpdate = false;
  return true;
}

void pushBullet(const char *pTitle, const char *pBody)
{
  WiFiClientSecure client;
  const char host[] = "api.pushbullet.com";
  const char url[] = "/v2/pushes";

  if (!client.connect(host, 443))
  {
    event.print("PushBullet connection failed");
    return;
  }

  String data = "{\"type\": \"note\", \"title\": \"";
  data += pTitle;
  data += "\", \"body\": \"";
  data += pBody;
  data += "\"}";

  client.print(String("POST ") + url + " HTTP/1.1\r\n" +
              "Host: " + host + "\r\n" +
              "Content-Type: application/json\r\n" +
              "Access-Token: " + ee.pbToken + "\r\n" +
              "User-Agent: Arduino\r\n" +
              "Content-Length: " + data.length() + "\r\n" + 
              "Connection: close\r\n\r\n" +
              data + "\r\n\r\n");
 
  int i = 0;
  while (client.connected() && ++i < 10)
  {
    String line = client.readStringUntil('\n');
    Serial.println(line);
  }
}
