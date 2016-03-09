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

#include <Wire.h>
#include <OneWire.h>
#include "ssd1306_i2c.h"
#include "icons.h"
#include <Time.h>

#include <WiFiClient.h>
#include <EEPROM.h>
#include <ESP8266mDNS.h>
#include "WiFiManager.h"
#include <ESP8266WebServer.h>

#define USEPIC // For PIC control

#define HCSR04  // For the ultrasonic rangefinder

const char *controlPassword = "password"; // device password for modifying any settings
const char *serverFile = "GarageDoor";    // Creates /iot/GarageDoor.php
int serverPort = 84;                    // port fwd for fwdip.php
const char *myHost = "www.yourdomain.com"; // php forwarding/time server

extern "C" {
  #include "user_interface.h" // Needed for deepSleep which isn't used
}

#define TOP_ACC    0  // direct pin on remote header (not used)
#define ESP_LED    2  // low turns on ESP blue LED
#define HEARTBEAT  2
#define ECHO      13  // the voltage divider to the bottom corner pin (short R7, don't use R8)
#define TRIG      12  // the direct pin (bottom 3 pin header)
#define RANGE_2   14
#define REMOTE    15
#define RANGE_1   16

uint32_t lastIP;
int nWrongPass;

SSD1306 display(0x3c, 5, 4); // Initialize the oled display for address 0x3c, sda=5, sdc=4
bool bDisplay_on = true;
bool bNeedUpdate = true;
bool bProgram;

WiFiManager wifi(0);  // AP page:  192.168.4.1
extern MDNSResponder mdns;
ESP8266WebServer server( serverPort );
#define CLIENTS 4
WiFiClient eventClient[CLIENTS];

int httpPort = 80; // may be modified by open AP scan

struct eeSet // EEPROM backed data
{
  uint16_t size;          // if size changes, use defauls
  uint16_t sum;           // if sum is diiferent from memory struct, write
  char     dataServer[64];
  uint8_t  dataServerPort; // may be modified by listener (dataHost)
  int8_t   tz;            // Timezone offset from your server
  uint16_t interval;
  uint16_t nCarThresh;
  uint16_t nDoorThresh;
  uint16_t alarmTimeout;
  uint16_t closeTimeout;
};
eeSet ee = { sizeof(eeSet), 0xAAAA,
  "192.168.0.189", 83,  2, 60*60, // dataServer, port, TZ, interval
  500, 500,       // thresholds
  5*60,          // alarmTimeout
  30            // closeTimeout
};

uint8_t hour_save, sec_save;
int ee_sum; // sum for checking if any setting has changed
bool bDoorOpen;
bool bCarIn;
int logCounter;
uint16_t doorVal;
uint16_t carVal;
uint16_t doorOpenTimer;

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
  int val;
  bool bUpdateTime = false;
  bool bRemote = false;
  bool ipSet = false;
  eeSet save;
  memcpy(&save, &ee, sizeof(ee));

  Serial.println("parseArgs");

  for ( uint8_t i = 0; i < server.args(); i++ ) {
    server.arg(i).toCharArray(temp, 100);
    String s = wifi.urldecode(temp);
    Serial.println( i + " " + server.argName ( i ) + ": " + s);
    bool which = (tolower(server.argName(i).charAt(1) ) == 'n') ? 1:0;

    switch( server.argName(i).charAt(0)  )
    {
      case 'k': // key
          password = s;
          break;
      case 'Z': // TZ
          ee.tz = s.toInt();
          bUpdateTime = true; // refresh current time
          break;
      case 'C': // close timeout (set a bit higher than it takes to close)
          ee.closeTimeout = s.toInt();
          break;
      case 'T': // alarm timeout
          ee.alarmTimeout = s.toInt();
          break;
      case 'i': // ?ip=server&port=80&int=60&key=password (htTp://192.168.0.197:84/s?ip=192.168.0.189&port=83&int=1800&Timeout=600&key=password)
          if(which) // interval
          {
            ee.interval = s.toInt();
          }
          else // ip
          {
            s.toCharArray(ee.dataServer, 64); // todo: parse into domain/URI
            Serial.print("Server ");
            Serial.println(ee.dataServer);
           }
           break;
      case 'L':           // Ex set car=290, door=500 : hTtp://192.168.0.190:84?Ln=500&Ld=290&key=password
           if(which) // Ln
           {
             ee.nDoorThresh = s.toInt();
           }
           else // Ld
           {
             ee.nCarThresh = s.toInt();
           }
           break;
      case 'D': // Door (pulse the output)
            bRemote = true;
            break;
      case 'p': // port
          ee.dataServerPort = s.toInt();
          break;
      case 'O': // OLED
          bDisplay_on = s.toInt() ? true:false;
          break;
      case 'B':   // for PIC programming
          if(s.toInt())
          {
              pinMode(REMOTE, OUTPUT);
              pinMode(HEARTBEAT, OUTPUT);
              Serial.println("Outputs enabled");
              bProgram = false;
          }
          else
          {
              pinMode(REMOTE, INPUT_PULLUP);
              pinMode(HEARTBEAT, INPUT);
              Serial.println("Program mode enabled");
              bProgram = true;
          }
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
    ctSendIP(false, ip); // log attempts
    bRemote = false;
  }

  if(nWrongPass) memcpy(&ee, &save, sizeof(ee)); // undo any changes

  lastIP = ip;
  if(bRemote)
     pulseRemote();
  if(bUpdateTime)
    ctSetIp();
}

void handleRoot() // Main webpage interface
{
  Serial.println("handleRoot");

  parseArgs();

    String page = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"/>"
      "<title>WiFi Garage Door Opener</title>"
      "<style>div,input {margin-bottom: 5px;}body{width:260px;display:block;margin-left:auto;margin-right:auto;text-align:right;font-family: Arial, Helvetica, sans-serif;}}</style>"

    "<script src=\"http://ajax.googleapis.com/ajax/libs/jquery/1.3.2/jquery.min.js\" type=\"text/javascript\" charset=\"utf-8\"></script>"
    "<script type=\"text/javascript\">"
    "function startEvents()"
    "{"
      "eventSource = new EventSource(\"events\");"
      "eventSource.addEventListener('open', function(e){},false);"
      "eventSource.addEventListener('error', function(e){},false);"
      "eventSource.addEventListener('state',function(e){"
        "data = JSON.parse(e.data);"
        "document.all.car.innerHTML = data.car?\"IN\":\"OUT\";"
        "document.all.door.innerHTML = data.door?\"OPEN\":\"CLOSED\";"
        "document.all.doorBtn.value = data.door?\"Close\":\"Open\";"
      "},false)"
    "}"
    "function setDoor(){"
    "$.post(\"s\", { params: 'D: 0', key: document.all.myKey.value })"
    "}"
    "</script>"

      "<body onload=\"{"
      "key = localStorage.getItem('key'); if(key!=null) document.getElementById('myKey').value = key;"
      "for(i=0;i<document.forms.length;i++) document.forms[i].elements['key'].value = key;"
      "startEvents();}\">";

    page += "<h3>WiFi Garage Door Opener </h3>"
            "<table align=\"right\">"
            "<tr><td>";
    page += timeFmt(true, true);
    page += "</td><td>";
    page += "<input type=\"button\" value=\"Open\" id=\"doorBtn\" onClick=\"{setDoor()}\">";
    page += "</td></tr>"
          "<tr><td align=\"center\">Garage</td>"
          "<td align=\"center\">Car</td>"
          "</tr><tr>"
          "<td align=\"center\">"
          "<div id=\"door\">";
    page += bDoorOpen ? "OPEN" : "CLOSED";
    page += "</div></td>"
            "<td align=\"center\">"
            "<div id=\"car\">";
    page += bCarIn ? "IN" : "OUT";
    page += "</div></td></tr>"
            "<tr><td colspan=2>" // unused row
            "</td>"
            "</tr><tr><td align=\"center\">Timeout</td><td align=\"center\">Timezone</td></tr>"
            "<tr><td>";
    page += valButton("C", String(ee.closeTimeout) );
    page += "</td><td>";  page += valButton("Z", String(ee.tz) );
    page += "</td></tr></table>"

            "<input id=\"myKey\" name=\"key\" type=text size=50 placeholder=\"password\" style=\"width: 150px\">"
            "<input type=\"button\" value=\"Save\" onClick=\"{localStorage.setItem('key', key = document.all.myKey.value)}\">"
            "<br><small>Logged IP: ";
    page += ipString(server.client().remoteIP());
    page += "</small><br></body></html>";

    server.send ( 200, "text/html", page );
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

// Set sec to 60 to remove seconds
String timeFmt(bool do_sec, bool do_12)
{
  String r = "";
  if(hourFormat12() <10) r = " ";
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
  if(do_12)
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
  page += "}";

  server.send ( 200, "text/json", page );
}

// event streamer (assume keep-alive)
void handleEvents()
{
  Serial.println("handleEvents");
  server.send( 200, "text/event-stream", "" );

  for(int i = 0; i < CLIENTS; i++)
    if(eventClient[i].connected() == 0)
    {
      Serial.print("send OK ");
      Serial.println(i);
      eventClient[i] = server.client();
      eventClient[i].print(":ok\n");
      break;
    }
}

void eventHeartbeat()
{
  static uint8_t cnt = 0;
  if(++cnt < 10) // let's try 10 seconds
    return;
  cnt = 0;

  for(int i = 0; i < CLIENTS; i++)
    if(eventClient[i].connected())
     eventClient[i].print("\n");
}

void eventPush(String s)
{
  for(int i = 0; i < CLIENTS; i++)
    if(eventClient[i].connected())
    {
      eventClient[i].print("event: state\n");
      eventClient[i].println("data: " + s + "\n");
    }
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

#ifdef HCSR04

volatile unsigned long range = 1;

void echoISR()
{
  static unsigned long start;

  if(digitalRead(ECHO) == HIGH) // count from rising edge to falling edge
    start = micros();
  else
    range = (micros() - start) >> 2; // don't need any specific value
}
#endif

void setup()
{
  pinMode(RANGE_1, OUTPUT);
  pinMode(RANGE_2, OUTPUT);
  pinMode(TRIG, OUTPUT);      // HC-SR04 trigger
  digitalWrite(RANGE_1, LOW); // LOW = off (do not turn both on at the same time)
  digitalWrite(RANGE_2, LOW);
  pinMode(REMOTE, OUTPUT);
  pinMode(HEARTBEAT, OUTPUT);
  digitalWrite(REMOTE, HIGH); // watchdog disabled (in case of config)
  digitalWrite(HEARTBEAT, LOW);

  digitalWrite(TRIG, LOW);

  // initialize dispaly
  display.init();

  display.clear();
  display.display();

  Serial.begin(115200);
//  delay(3000);
  Serial.println();
  Serial.println();

  wifi.autoConnect("ESP8266");
  eeRead(); // don't access EE before WiFi init

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  digitalWrite(HEARTBEAT, HIGH);
  if ( mdns.begin ( "esp8266", WiFi.localIP() ) ) {
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

  digitalWrite(REMOTE, LOW); // enable watchdog

  logCounter = 20;
  digitalWrite(HEARTBEAT, HIGH);

  ctSendIP(true, WiFi.localIP());

  digitalWrite(RANGE_2, LOW);   // enable ADC 1
  digitalWrite(RANGE_1, HIGH);

#ifdef HCSR04
  attachInterrupt(ECHO, echoISR, CHANGE);
#endif
}

void loop()
{
  static uint8_t tog = 0;
  static uint8_t cnt = 0;
#define ANA_AVG 24
  static uint16_t vals[2][ANA_AVG];
  static uint8_t ind[2];
  bool bSkip = false;
  bool bNew;

  mdns.update();
  server.handleClient();

  if(sec_save != second()) // only do stuff once per second (loop is maybe 20-30 Hz)
  {
    sec_save = second();

#ifndef HCSR04
    bSkip = true; // skip first read (needs time to start)
    switch(cnt)
    {
      case 0: // read 1 (middle header)
#endif                  // this section reads only IR rangefinder 1
        doorVal = 0;
        for(int i = 0; i < ANA_AVG; i++)
          doorVal += vals[0][i];
        doorVal /= ANA_AVG;
        bNew = (doorVal > ee.nDoorThresh) ? true:false;
        if(bNew != bDoorOpen)
        {
            bDoorOpen = bNew;
            doorOpenTimer = bDoorOpen ? ee.alarmTimeout : 0;
            ctSendLog(false); // send all changes in door status
        }
//        Serial.print("Door: ");
//        Serial.println(doorVal);
#ifndef HCSR04
        digitalWrite(RANGE_1, LOW);   // enable ADC 2
        digitalWrite(RANGE_2, HIGH);
        cnt++;
        tog = 1;
        break;
      case 1: // read 2 (right header)
        cnt++;
        break;
      case 2:
        carVal = 0;
        for(int i = 0; i < ANA_AVG; i++)
          carVal += vals[1][i];
        carVal /= ANA_AVG;
        bCarIn = (carVal > ee.nCarThresh) ? true:false; // higher is closer
//        Serial.print("Car: ");
//        Serial.println(carVal);
        digitalWrite(RANGE_2, LOW);   // enable ADC 1
        digitalWrite(RANGE_1, HIGH);
        cnt++;
        tog = 0;
        break;
      case 3: // 2 seconds each now
        cnt = 0;
        break;
    }
#endif

#ifdef HCSR04
    carVal = 0;
    for(int i = 0; i < ANA_AVG; i++)
      carVal += vals[1][i];
    carVal /= ANA_AVG;

    bNew = (carVal < ee.nCarThresh) ? true:false; // lower is closer

    if(bNew != bCarIn)
    {
      bCarIn = bNew;
      ctSendLog(false);
    }
#endif

    if (hour_save != hour()) // update our IP and time daily (at 2AM for DST)
    {
      display.init();
      if( (hour_save = hour()) == 2)
        bNeedUpdate = true;
    }

    if(bNeedUpdate)
      if( ctSetIp() )
        bNeedUpdate = false;

    if(nWrongPass)
      nWrongPass--;

    if(logCounter)
    {
      if(--logCounter == 0)
      {
        logCounter = ee.interval;
        ctSendLog(false); // no really needed, but as a heartbeat
        eeWrite(); // update EEPROM if needed while we're at it (give user time to make many adjustments)
      }
    }
    eventHeartbeat();
    if(doorOpenTimer)
    {
      if(--doorOpenTimer == 0)
      {
        ctSendLog(true); // Send to server as alert so it can send a Pushbullet
      }
    }
    if(bProgram == false)
      digitalWrite(HEARTBEAT, !digitalRead(HEARTBEAT));
  }

  DrawScreen();

  if(!bSkip)
  {
    vals[tog][ind[tog]] = analogRead(A0); // read current IR sensor value into current circle buf
    if(++ind[tog] >= ANA_AVG) ind[tog] = 0;
  }

#ifdef HCSR04
    if(range)
    {
      vals[1][ind[1]] = range; // read current IR sensor value into current circle buf
      if(++ind[1] >= ANA_AVG) ind[1] = 0;
      range = 0;
      delay(20);
      digitalWrite(TRIG, HIGH); // pulse the rangefinder
      delayMicroseconds(10);
      digitalWrite(TRIG, LOW);
    }
#endif
}

void DrawScreen()
{
  static int16_t ind;
  // draw the screen here
  display.clear();

  if(bDisplay_on)
  {
    display.setFontScale2x2(false);
    display.drawString( 8, 22, "Door");
    display.drawString(80, 22, "Car");

    display.setFontScale2x2(true);

    String s = timeFmt(true, true);
    s += "  ";
    s += dayShortStr(weekday());
    s += " ";
    s += String(day());
    s += " ";
    s += monthShortStr(month());
    s += "  ";
    int len = s.length();
    s = s + s;

    int w = display.drawPropString(ind, 0, s );
    if( --ind < -(w)) ind = 0;

    display.drawPropString( 2, 34, bDoorOpen ? "Open":"Closed" );
    display.drawPropString(80, 34, bCarIn ? "In":"Out" );
  }
  display.display();
}

void pulseRemote()
{
  if(bProgram == true)
    return;
  if(bDoorOpen == false) // closing door
  {
    doorOpenTimer = ee.closeTimeout; // set the short timeout 
  }

  for(int i = 0; i < 10; i++)
  {
    digitalWrite(REMOTE, HIGH);
    delay(10);
    digitalWrite(REMOTE, LOW);
    delay(10);
  }
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

// Send logging data to a server.  This sends JSON formatted data to my local PC, but change to anything needed.
void ctSendLog(bool bAlert)
{
  String s = "{\"door\": ";
  s += bDoorOpen;
  s += ", \"car\": ";
  s += bCarIn;
  s += ", \"alert\": ";
  s += bAlert;
  s += "}";
  ctSend(String("/s?gdoLog=") + s);

  eventPush(s);
}

// Send local IP on start for comm, or bad password attempt IP when caught
void ctSendIP(bool local, uint32_t ip)
{
  String s = local ? "/s?gdoIP=\"" : "/s?gdoHackIP=\"";
  s += ipString(ip);

  if(local)
  {
    s += ":";
    s += serverPort;
  }
  s += "\"";

  ctSend(s);
}

// Send stuff to a server.
void ctSend(String s)
{
  if(ee.dataServer[0] == 0) return;
  WiFiClient client;
  if (!client.connect(ee.dataServer, ee.dataServerPort)) {
    Serial.println("dataServer connection failed");
    Serial.println(ee.dataServer);
    Serial.println(ee.dataServerPort);
    delay(100);
    return;
  }
  Serial.println("dataServer connected");

  // This will send the request to the server
  client.print(String("GET ") + s + " HTTP/1.1\r\n" +
               "Host: " + ee.dataServer + "\r\n" + 
               "Connection: close\r\n\r\n");

  delay(10);
 
  // Read all the lines of the reply from server and print them to Serial
  while(client.available()){
    String line = client.readStringUntil('\r');
    Serial.print(line);
  }

  Serial.println();
//  Serial.println("closing connection.");
  client.stop();
}

// Setup a php script on my server to send traffic here from /iot/waterbed.php, plus sync time
// PHP script: https://github.com/CuriousTech/ESP07_Multi/blob/master/fwdip.php
bool ctSetIp()
{
  WiFiClient client;
  if (!client.connect(myHost, httpPort))
  {
    Serial.println("Host ip connection failed");
    delay(100);
    return false;
  }

  String url = "/fwdip.php?name=";
  url += serverFile;
  url += "&port=";
  url += serverPort;

  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: " + myHost + "\r\n" + 
               "Connection: close\r\n\r\n");

  delay(10);
  String line = client.readStringUntil('\n');
  // Read all the lines of the reply from server
  while(client.available())
  {
    line = client.readStringUntil('\n');
    line.trim();
    if(line.startsWith("IP:")) // I don't need my global IP
    {
    }
    else if(line.startsWith("Time:")) // Time from the server in a simple format
    {
      tmElements_t tm;
      tm.Year   = line.substring(6,10).toInt() - 1970;
      tm.Month  = line.substring(11,13).toInt();
      tm.Day    = line.substring(14,16).toInt();
      tm.Hour   = line.substring(17,19).toInt();
      tm.Minute = line.substring(20,22).toInt();
      tm.Second = line.substring(23,25).toInt();

      unsigned long t = makeTime(tm);
      t += 3600 * (ee.tz + DST()); // offset in hours
      setTime(t);  // set time

      Serial.println("Time updated");
    }
  }
 
  client.stop();
  return true;
}

uint8_t DST() // 2016 starts 2AM Mar 13, ends Nov 6
{
  uint8_t m = month();
  int8_t d = day();
  int8_t dow = weekday();
  if ((m  >  3 && m < 11 ) || 
      (m ==  3 && d >= 8 && dow == 0 && hour() >= 2) ||  // DST starts 2nd Sunday of March;  2am
      (m == 11 && d <  8 && dow >  0) ||
      (m == 11 && d <  8 && dow == 0 && hour() < 2))   // DST ends 1st Sunday of November; 2am
    return 1;
 return 0;
}
