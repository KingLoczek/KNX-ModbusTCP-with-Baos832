#include <stdint.h>
#ifndef __KNX_BAOS_H__
#define __KNX_BAOS__

#include "Arduino.h"
#define serialKNX Serial1

void knxBaos_initHardware();
void knxBaos_initLayer_Link();
void knxBaos_initLayer_Baos();

void knxBaos_sendRaw(uint8_t *data, uint8_t length);
void knxBaos_sendCmd(uint8_t *data, uint8_t length);
void knxBaos_sendBaosCmd(uint8_t *data, uint8_t length);
uint8_t knxBaos_calculateC(uint8_t *data, uint8_t len);

void knxBaos_setDataPointBinary(uint16_t dataPointIndex, uint8_t binary);
void knxBaos_setDataPoint2Byte(uint16_t dataPointIndex, uint16_t value);
// void knxBaos_getDataPointValue(uint16_t dataPointIndex, uint16_t maxDataPoints,uint8_t filter);
#endif