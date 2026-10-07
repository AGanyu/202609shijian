#include "ZComDef.h"
#include "OSAL.h"
#include "AF.h"
#include "hal_uart.h"
#include "zcl_genericapp.h"
#include "gateway_uart.h"

#if ZG_BUILD_COORDINATOR_TYPE

#define GATEWAY_UART_PORT          HAL_UART_PORT_0
#define GATEWAY_UART_RX_SIZE       128
#define GATEWAY_UART_TX_SIZE       256
#define GATEWAY_UART_LINE_SIZE     144
#define GATEWAY_CLUSTER_ID         0xFC00
#define GATEWAY_ENDPOINT           GENERICAPP_ENDPOINT
#define GATEWAY_MAX_ZB_PAYLOAD     64

static uint8 gateway_line[GATEWAY_UART_LINE_SIZE];
static uint8 gateway_line_length;
static uint8 gateway_transaction_id;
static endPointDesc_t gateway_source_endpoint =
{
  GATEWAY_ENDPOINT,
  0,
  &zclGenericApp_TaskID,
  &zclGenericApp_SimpleDesc,
  noLatencyReqs
};

static void gateway_uart_callback(uint8 port, uint8 event)
{
  if (port == GATEWAY_UART_PORT &&
      (event & (HAL_UART_RX_FULL | HAL_UART_RX_ABOUT_FULL |
                HAL_UART_RX_TIMEOUT)) != 0) {
    osal_set_event(zclGenericApp_TaskID, GATEWAY_UART_RX_EVT);
  }
}

static void gateway_write(const uint8 *data, uint16 length)
{
  if (length != 0) HalUARTWrite(GATEWAY_UART_PORT, (uint8 *)data, length);
}

static void gateway_write_text(const char *text)
{
  uint16 length = 0;
  while (text[length] != 0) ++length;
  gateway_write((const uint8 *)text, length);
}

static uint8 gateway_hex_digit(uint8 value)
{
  value &= 0x0F;
  return (value < 10) ? (uint8)('0' + value) : (uint8)('A' + value - 10);
}

static void gateway_write_hex8(uint8 value)
{
  uint8 text[2];
  text[0] = gateway_hex_digit((uint8)(value >> 4));
  text[1] = gateway_hex_digit(value);
  gateway_write(text, 2);
}

static void gateway_write_hex16(uint16 value)
{
  gateway_write_hex8((uint8)(value >> 8));
  gateway_write_hex8((uint8)value);
}

static void gateway_write_u16(uint16 value)
{
  uint8 text[5];
  uint8 position = sizeof(text);

  do {
    text[--position] = (uint8)('0' + (value % 10));
    value /= 10;
  } while (value != 0 && position != 0);
  gateway_write(&text[position], (uint16)(sizeof(text) - position));
}

static void gateway_write_i16(int16 value)
{
  if (value < 0) {
    gateway_write_text("-");
    value = (int16)-value;
  }
  gateway_write_u16((uint16)value);
}

static uint8 gateway_is_space(uint8 value)
{
  return (value == ' ' || value == '\t') ? TRUE : FALSE;
}

static uint8 gateway_hex_value(uint8 value, uint8 *result)
{
  if (value >= '0' && value <= '9') {
    *result = (uint8)(value - '0');
    return TRUE;
  }
  if (value >= 'A' && value <= 'F') {
    *result = (uint8)(value - 'A' + 10);
    return TRUE;
  }
  if (value >= 'a' && value <= 'f') {
    *result = (uint8)(value - 'a' + 10);
    return TRUE;
  }
  return FALSE;
}

static uint8 gateway_parse_hex16(const uint8 **cursor, uint16 *value)
{
  uint8 i;
  uint8 nibble;

  *value = 0;
  for (i = 0; i < 4; ++i) {
    if (!gateway_hex_value(*(*cursor)++, &nibble)) return FALSE;
    *value = (uint16)((*value << 4) | nibble);
  }
  return TRUE;
}

static uint8 gateway_parse_hex_payload(const uint8 **cursor,
                                       uint8 *payload, uint8 *length)
{
  uint8 high;
  uint8 low;
  uint8 count = 0;

  while (**cursor != 0 && !gateway_is_space(**cursor)) {
    if (count >= GATEWAY_MAX_ZB_PAYLOAD ||
        !gateway_hex_value(*(*cursor)++, &high) ||
        **cursor == 0 ||
        !gateway_hex_value(*(*cursor)++, &low)) {
      return FALSE;
    }
    payload[count++] = (uint8)((high << 4) | low);
  }
  *length = count;
  return (count != 0) ? TRUE : FALSE;
}

static void gateway_send_zigbee(uint16 destination, const uint8 *payload,
                                uint8 length)
{
  afAddrType_t address;
  uint8 status;

  address.addrMode = afAddr16Bit;
  address.addr.shortAddr = destination;
  address.endPoint = GATEWAY_ENDPOINT;
  address.panId = 0;
  status = AF_DataRequest(&address, &gateway_source_endpoint,
                          GATEWAY_CLUSTER_ID, length, (uint8 *)payload,
                          &gateway_transaction_id, AF_ACK_REQUEST,
                          AF_DEFAULT_RADIUS);
  if (status == ZSuccess) {
    gateway_write_text("TX_OK\r\n");
  } else {
    gateway_write_text("TX_ERR,");
    gateway_write_hex8(status);
    gateway_write_text("\r\n");
  }
}

static void gateway_process_line(void)
{
  const uint8 *cursor = gateway_line;
  uint16 destination;
  uint8 payload[GATEWAY_MAX_ZB_PAYLOAD];
  uint8 length;

  if (gateway_line_length == 4 && gateway_line[0] == 'P' &&
      gateway_line[1] == 'I' && gateway_line[2] == 'N' &&
      gateway_line[3] == 'G') {
    gateway_write_text("PONG\r\n");
    return;
  }
  if (gateway_line_length < 5 || gateway_line[0] != 'S' ||
      gateway_line[1] != 'E' || gateway_line[2] != 'N' ||
      gateway_line[3] != 'D' || !gateway_is_space(gateway_line[4])) {
    gateway_write_text("ERR,BAD_COMMAND\r\n");
    return;
  }

  cursor += 5;
  while (gateway_is_space(*cursor)) ++cursor;
  if (!gateway_parse_hex16(&cursor, &destination)) {
    gateway_write_text("ERR,BAD_ADDRESS\r\n");
    return;
  }
  while (gateway_is_space(*cursor)) ++cursor;
  if (!gateway_parse_hex_payload(&cursor, payload, &length)) {
    gateway_write_text("ERR,BAD_PAYLOAD\r\n");
    return;
  }
  gateway_send_zigbee(destination, payload, length);
}

void GatewayUartInit(void)
{
  halUARTCfg_t config;

  config.configured = TRUE;
  config.baudRate = HAL_UART_BR_115200;
  config.flowControl = FALSE;
  config.flowControlThreshold = 0;
  config.idleTimeout = 6;
  config.rx.maxBufSize = GATEWAY_UART_RX_SIZE;
  config.rx.pBuffer = NULL;
  config.tx.maxBufSize = GATEWAY_UART_TX_SIZE;
  config.tx.pBuffer = NULL;
  config.intEnable = TRUE;
  config.rxChRvdTime = 0;
  config.callBackFunc = gateway_uart_callback;
  HalUARTOpen(GATEWAY_UART_PORT, &config);
}

void GatewayUartPoll(void)
{
  uint8 byte;

  while (Hal_UART_RxBufLen(GATEWAY_UART_PORT) != 0) {
    if (HalUARTRead(GATEWAY_UART_PORT, &byte, 1) != 1) break;
    if (byte == '\r') continue;
    if (byte == '\n') {
      if (gateway_line_length != 0) gateway_process_line();
      gateway_line_length = 0;
    } else if (gateway_line_length < GATEWAY_UART_LINE_SIZE - 1) {
      gateway_line[gateway_line_length++] = byte;
      gateway_line[gateway_line_length] = 0;
    } else {
      gateway_line_length = 0;
      gateway_write_text("ERR,LINE_TOO_LONG\r\n");
    }
  }
}

void GatewayUartHandleIncoming(afIncomingMSGPacket_t *packet)
{
  uint8 *data;
  uint16 source;
  uint16 length;

  if (packet->clusterId != GATEWAY_CLUSTER_ID ||
      packet->cmd.Data == NULL || packet->cmd.DataLength == 0) return;

  source = packet->srcAddr.addr.shortAddr;
  data = packet->cmd.Data;
  length = packet->cmd.DataLength;
  gateway_write_text("ZB,src=0x");
  gateway_write_hex16(source);
  gateway_write_text(",len=");
  gateway_write_u16(length);
  gateway_write_text(",lqi=");
  gateway_write_u16(packet->LinkQuality);
  gateway_write_text(",rssi=");
  gateway_write_i16(packet->rssi);
  gateway_write_text(",data=");
  while (length-- != 0) gateway_write_hex8(*data++);
  gateway_write_text("\r\n");
}

#else

void GatewayUartInit(void) {}
void GatewayUartPoll(void) {}
void GatewayUartHandleIncoming(afIncomingMSGPacket_t *packet) { (void)packet; }

#endif
