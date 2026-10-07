#include "ZComDef.h"
#include "OSAL.h"
#include "hal_uart.h"
#include "sht30.h"
#include "zcl_genericapp.h"

/* The photographed module is a UART sensor, not a raw I2C SHT30. */
#define SHT30_UART_PORT       HAL_UART_PORT_1
#define SHT30_UART_RX_BUFFER  96
#define SHT30_LINE_BUFFER     40

static uint8 sht30_line[SHT30_LINE_BUFFER];
static uint8 sht30_line_length;
static int16 sht30_temperature;
static uint16 sht30_humidity;
static uint8 sht30_measurement_valid;

static uint8 sht30_parse_decimal(const uint8 **cursor, int16 *value)
{
  uint16 whole;
  uint8 fraction;
  uint8 digits;
  uint8 negative;
  const uint8 *p;

  p = *cursor;
  negative = 0;
  if (*p == '-')
  {
    negative = 1;
    p++;
  }
  whole = 0;
  digits = 0;
  while ((*p >= '0') && (*p <= '9'))
  {
    whole = (uint16)(whole * 10 + (*p - '0'));
    p++;
    digits++;
  }
  if ((digits == 0) || (*p != '.'))
  {
    return FALSE;
  }
  p++;
  if ((*p < '0') || (*p > '9'))
  {
    return FALSE;
  }
  fraction = (uint8)(*p - '0');
  p++;
  *value = (int16)(whole * 100 + fraction * 10);
  if (negative)
  {
    *value = (int16)-(*value);
  }
  *cursor = p;
  return TRUE;
}

static uint8 sht30_parse_line(void)
{
  const uint8 *p;
  int16 humidity;
  int16 temperature;

  /* Observed module format: R:055.4RH 026.5C\r\n */
  if ((sht30_line_length < 12) ||
      (sht30_line[0] != 'R') || (sht30_line[1] != ':'))
  {
    return FALSE;
  }

  p = &sht30_line[2];
  if (!sht30_parse_decimal(&p, &humidity) ||
      (p[0] != 'R') || (p[1] != 'H'))
  {
    return FALSE;
  }
  p += 2;
  if (*p != ' ')
  {
    return FALSE;
  }
  p++;
  if (!sht30_parse_decimal(&p, &temperature) || (*p != 'C'))
  {
    return FALSE;
  }
  if ((humidity < 0) || (humidity > 10000))
  {
    return FALSE;
  }
  sht30_humidity = (uint16)humidity;
  sht30_temperature = temperature;
  sht30_measurement_valid = TRUE;
  return TRUE;
}

static void sht30_uart_callback(uint8 port, uint8 event)
{
  (void)port;
  if ((event & (HAL_UART_RX_FULL | HAL_UART_RX_ABOUT_FULL |
                HAL_UART_RX_TIMEOUT)) != 0)
  {
    osal_set_event(zclGenericApp_TaskID, SHT30_UART_RX_EVT);
  }
}

void SHT30_Init(void)
{
  halUARTCfg_t config;

  sht30_line_length = 0;
  sht30_temperature = 0;
  sht30_humidity = 0;
  sht30_measurement_valid = FALSE;

  osal_memset(&config, 0, sizeof(config));
  config.configured = TRUE;
  config.baudRate = HAL_UART_BR_9600;
  config.flowControl = FALSE;
  config.flowControlThreshold = 0;
  config.rx.maxBufSize = SHT30_UART_RX_BUFFER;
  config.tx.maxBufSize = 16;
  config.idleTimeout = 6;
  config.intEnable = TRUE;
  config.callBackFunc = sht30_uart_callback;
  HalUARTOpen(SHT30_UART_PORT, &config);
}

void SHT30_ProcessUart(void)
{
  uint8 buffer[32];
  uint16 length;
  uint16 i;
  uint8 ch;

  length = HalUARTRead(SHT30_UART_PORT, buffer, sizeof(buffer));
  for (i = 0; i < length; i++)
  {
    ch = buffer[i];
    if (ch == '\n')
    {
      (void)sht30_parse_line();
      sht30_line_length = 0;
    }
    else if ((ch >= 0x20) && (ch <= 0x7E))
    {
      if (sht30_line_length < (SHT30_LINE_BUFFER - 1))
      {
        sht30_line[sht30_line_length++] = ch;
      }
      else
      {
        /* Drop startup garbage or an overlong frame until the next LF. */
        sht30_line_length = 0;
      }
    }
  }
}

uint8 SHT30_ReadMeasurement(int16 *temperatureCentiC, uint16 *humidityCentiPct)
{
  if ((temperatureCentiC == NULL) || (humidityCentiPct == NULL) ||
      !sht30_measurement_valid)
  {
    return SHT30_ERROR;
  }
  *temperatureCentiC = sht30_temperature;
  *humidityCentiPct = sht30_humidity;
  return SHT30_OK;
}
