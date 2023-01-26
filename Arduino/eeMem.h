#ifndef EEMEM_H
#define EEMEM_H

#include <Arduino.h>

#define EESIZE (offsetof(eeMem, end) - offsetof(eeMem, size) )

class eeMem
{
public:
  eeMem();
  void update(void);
private:
  uint16_t Fletcher16( uint8_t* data, int count);
public:
  uint16_t size = EESIZE;    // if size changes, use defauls
  uint16_t sum = 0xAAAA;           // if sum is diiferent from memory struct, write
  char     szSSID[32] = "";
  char     szSSIDPassword[64] = "";
  int8_t   tz = 51;            // Timezone offset
  uint8_t  useTime = 0;
  uint16_t nCarThresh = 150; // cm
  uint16_t nDoorThresh = 150;
  uint16_t alarmTimeout = 5 * 60; // seconds
  uint16_t closeTimeout = 60;
  int8_t   tempCal = 0;
  bool     bEnableOLED = true;
  uint16_t delayClose = 10;
  uint16_t rate = 55;
  char     pbToken[40] = "pushbullet token";
  char     szControlPassword[32] =  "password";
  uint8_t  hostIP[4] = {192,168,31,100};
  uint16_t hostPort = 80;
  uint16_t res = 0;
  uint8_t end;
};

extern eeMem ee;

#endif // EEMEM_H
