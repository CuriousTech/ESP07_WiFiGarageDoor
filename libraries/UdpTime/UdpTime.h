#ifndef UDPTIME_H
#define UDPTIME_H

#include <WiFiUDP.h>

class UdpTime
{
public:
  UdpTime();
  void start(void);
  bool check(int8_t tz);
  void DST(void);
  uint8_t getDST(void);
private:
#define NTP_PACKET_SIZE  48 // NTP time stamp is in the first 48 bytes of the message
  byte packetBuffer[ NTP_PACKET_SIZE]; //buffer to hold incoming and outgoing packets
  WiFiUDP Udp;
  bool _bInit;
  bool _bWaiting;
  uint8_t _dst;           // current dst
};

#endif // UDPTIME_H

