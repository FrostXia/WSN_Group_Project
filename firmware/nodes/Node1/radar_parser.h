#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
// Hi-Link LD2410B protocol V1.06: basic and engineering report frames.
class RadarParser {
  uint8_t buffer[128]{};
  size_t size=0;
  uint32_t lastByte=0;
  void drop() { memmove(buffer,buffer+1,--size); }
public:
  bool present=false, seen=false;
  uint32_t receivedAt=0;
  void feed(uint8_t value,uint32_t now) {
    if(size && uint32_t(now-lastByte)>100) size=0;
    lastByte=now;
    if(size==sizeof(buffer)) drop();
    buffer[size++]=value;
    const uint8_t head[]={0xf4,0xf3,0xf2,0xf1};
    const uint8_t tail[]={0xf8,0xf7,0xf6,0xf5};
    while(size>=4) {
      if(memcmp(buffer,head,4)) { drop(); continue; }
      if(size<6) return;
      unsigned length=buffer[4]|(unsigned(buffer[5])<<8);
      if(length<13 || length+10>sizeof(buffer)) { drop(); continue; }
      unsigned total=length+10;
      if(size<total) return;
      bool valid=(buffer[6]==1 || (buffer[6]==2 && length==13)) &&
        buffer[7]==0xaa && buffer[8]<=3 && buffer[total-6]==0x55 &&
        buffer[total-5]==0 && !memcmp(buffer+total-4,tail,4);
      if(!valid) { drop(); continue; }
      present=buffer[8]!=0; seen=true; receivedAt=now;
      memmove(buffer,buffer+total,size-total); size-=total;
    }
  }
  bool available(uint32_t now,uint32_t timeout) const {
    return seen && uint32_t(now-receivedAt)<timeout;
  }
};
