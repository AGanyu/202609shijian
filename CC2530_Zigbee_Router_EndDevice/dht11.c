#include "ZComDef.h"
#include "OnBoard.h"
#include "dht11.h"

#define DHT11_DATA_MASK              BV(DHT11_DATA_PIN)
#define DHT11_START_LOW_US           18000
#define DHT11_RESPONSE_TIMEOUT_US    100
#define DHT11_BIT_TIMEOUT_US         80
#define DHT11_SAMPLE_OFFSET_US       40

static void dht11_release_bus(void)
{
  /* P0.6 is GPIO input; the external resistor pulls the bus high. */
  P0SEL &= (uint8)~DHT11_DATA_MASK;
  P0DIR &= (uint8)~DHT11_DATA_MASK;
  P0INP &= (uint8)~DHT11_DATA_MASK;
}

static void dht11_drive_low(void)
{
  P0SEL &= (uint8)~DHT11_DATA_MASK;
  P0 &= (uint8)~DHT11_DATA_MASK;
  P0DIR |= DHT11_DATA_MASK;
}

static uint8 dht11_wait_level(uint8 high, uint16 timeoutUs)
{
  uint16 loops = (uint16)(timeoutUs / 2);

  while (loops-- != 0) {
    if (((P0 & DHT11_DATA_MASK) != 0) == (high != 0)) return TRUE;
    MicroWait(2);
  }
  return FALSE;
}

static uint8 dht11_read_byte(uint8 *value)
{
  uint8 bit;
  uint8 result = 0;

  for (bit = 0; bit < 8; ++bit) {
    /* Every data bit begins with approximately 50 us low. */
    if (!dht11_wait_level(FALSE, DHT11_BIT_TIMEOUT_US)) return FALSE;
    if (!dht11_wait_level(TRUE, DHT11_BIT_TIMEOUT_US)) return FALSE;
    MicroWait(DHT11_SAMPLE_OFFSET_US);
    result <<= 1;
    if ((P0 & DHT11_DATA_MASK) != 0) result |= 1;
    if (!dht11_wait_level(FALSE, DHT11_BIT_TIMEOUT_US)) return FALSE;
  }

  *value = result;
  return TRUE;
}

void DHT11_Init(void)
{
  dht11_release_bus();
}

uint8 DHT11_Read(uint16 *temperatureCentiC, uint16 *humidityCentiPct)
{
  uint8 data[5];
  uint8 index;
  uint8 checksum;

  dht11_drive_low();
  MicroWait(DHT11_START_LOW_US);
  dht11_release_bus();
  MicroWait(30);

  /* Sensor response: low, high, then the first data-bit low. */
  if (!dht11_wait_level(FALSE, DHT11_RESPONSE_TIMEOUT_US) ||
      !dht11_wait_level(TRUE, DHT11_RESPONSE_TIMEOUT_US) ||
      !dht11_wait_level(FALSE, DHT11_RESPONSE_TIMEOUT_US)) {
    dht11_release_bus();
    return DHT11_ERROR_TIMEOUT;
  }

  for (index = 0; index < 5; ++index) {
    if (!dht11_read_byte(&data[index])) {
      dht11_release_bus();
      return DHT11_ERROR_TIMEOUT;
    }
  }
  dht11_release_bus();

  checksum = (uint8)(data[0] + data[1] + data[2] + data[3]);
  if (checksum != data[4]) return DHT11_ERROR_CHECKSUM;

  /* DHT11 reports integral degrees and percent RH. */
  if (humidityCentiPct != NULL) {
    *humidityCentiPct = (uint16)data[0] * 100;
  }
  if (temperatureCentiC != NULL) {
    *temperatureCentiC = (uint16)data[2] * 100;
  }
  return DHT11_OK;
}
