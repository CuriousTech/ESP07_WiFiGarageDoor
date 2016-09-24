#ifndef EEMEM_H
#define EEMEM_H

#include <Arduino.h>

struct eeSet // EEPROM backed data
{
  uint16_t size;          // if size changes, use defauls
  uint16_t sum;           // if sum is diiferent from memory struct, write
  char     szSSID[32];
  char     szSSIDPassword[64];
  int8_t   tz;            // Timezone offset
  uint8_t  dst;
  uint16_t nCarThresh;
  uint16_t nDoorThresh;
  uint16_t alarmTimeout;
  uint16_t closeTimeout;
  int8_t   tempCal;
  bool     bEnableOLED;
  uint16_t delayClose;
  uint16_t res1[8];
  char     pbToken[40];
};

extern eeSet ee;

class eeMem
{
public:
  eeMem();
  void update(void);
private:
  uint16_t Fletcher16( uint8_t* data, int count);
};

extern eeMem eemem;

#endif // EEMEM_H
