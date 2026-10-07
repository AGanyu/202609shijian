#ifndef SHT30_H
#define SHT30_H

#include "ZComDef.h"

#define SHT30_OK                 0
#define SHT30_ERROR              1

/* The driver uses an open-drain GPIO implementation of I2C. */
void SHT30_Init(void);
void SHT30_ProcessUart(void);
uint8 SHT30_ReadMeasurement(int16 *temperatureCentiC, uint16 *humidityCentiPct);

#endif /* SHT30_H */
