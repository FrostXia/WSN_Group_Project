#pragma once
#include <math.h>
#include <stdint.h>
struct SensorReadings {
  float temperature=NAN, humidity=NAN, lux=NAN, mq2Voltage=NAN, dustVoltage=NAN;
  uint8_t motion=0, presence=0, valid=0;
};
void sensorsBegin();
void sensorsPoll();
SensorReadings sensorsRead();
