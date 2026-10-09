#ifndef DHT11_H
#define DHT11_H

#include "ZComDef.h"

#define DHT11_OK                     0
#define DHT11_ERROR_TIMEOUT          1
#define DHT11_ERROR_CHECKSUM         2

/* DHT11 DATA is connected to CC2530 P0.6. Use an external 4.7k-10k pull-up. */
#define DHT11_DATA_PIN               6

void DHT11_Init(void);
uint8 DHT11_Read(uint16 *temperatureCentiC, uint16 *humidityCentiPct);

#endif /* DHT11_H */
