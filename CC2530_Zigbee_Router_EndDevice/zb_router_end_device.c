#include "ZComDef.h"
#include "OSAL.h"
#include "AF.h"
#include "ZDApp.h"
#include "zcl_genericapp.h"
#include "zb_router_end_device.h"
#include "dht11.h"

#define ZB_RE_HELLO_PERIOD_MS          10000
#define ZB_RE_REPORT_PERIOD_MS          30000
#define ZB_RE_HELLO_RETRY_MS            5000
#define ZB_RE_ACK_TIMEOUT_MS            3000
#define ZB_RE_MAX_RETRIES               3
#define ZB_RE_BROADCAST_ADDR            0xFFFF

static uint8 zb_re_transaction_id;
static uint8 zb_re_sequence;
static uint8 zb_re_network_ready;
static uint8 zb_re_waiting_ack;
static uint8 zb_re_retry_count;
static uint16 zb_re_router_address;
static int16 zb_re_temperature_centi_c;
static uint16 zb_re_humidity_centi_pct;

static uint16 zb_re_last_source;
static int16 zb_re_last_temperature;
static uint16 zb_re_last_humidity;
static uint8 zb_re_last_sequence;
static uint8 zb_re_last_report_valid;

static endPointDesc_t zb_re_source_endpoint =
{
  ZB_RE_ENDPOINT,
  0,
  &zclGenericApp_TaskID,
  &zclGenericApp_SimpleDesc,
  noLatencyReqs
};

static uint8 zb_re_send(uint16 destination, uint8 *payload, uint8 length)
{
  afAddrType_t address;
  uint8 options;

  address.addrMode = afAddr16Bit;
  address.addr.shortAddr = destination;
  address.endPoint = ZB_RE_ENDPOINT;
  address.panId = 0;
  options = (destination == ZB_RE_BROADCAST_ADDR) ? 0 : AF_ACK_REQUEST;

  return AF_DataRequest(&address,
                        &zb_re_source_endpoint,
                        ZB_RE_CLUSTER_ID,
                        length,
                        payload,
                        &zb_re_transaction_id,
                        options,
                        AF_DEFAULT_RADIUS);
}

static void zb_re_send_hello(void)
{
  uint8 payload[3];

  if (!zb_re_network_ready) return;

  payload[0] = ZB_RE_CMD_HELLO;
  payload[1] = 0x01;
  payload[2] = zb_re_sequence++;
  (void)zb_re_send(ZB_RE_BROADCAST_ADDR, payload, sizeof(payload));
}

static void zb_re_send_report(void)
{
  uint8 payload[6];
  uint8 sensorStatus;
  uint16 temperature;
  uint16 sensedTemperature;
  uint16 sensedHumidity;

  if (!zb_re_network_ready || zb_re_router_address == ZB_RE_BROADCAST_ADDR) return;

  sensorStatus = DHT11_Read(&sensedTemperature, &sensedHumidity);
  if (sensorStatus != DHT11_OK) {
    osal_start_timerEx(zclGenericApp_TaskID,
                       ZB_RE_REPORT_EVT,
                       ZB_RE_REPORT_PERIOD_MS);
    return;
  }
  zb_re_temperature_centi_c = (int16)sensedTemperature;
  zb_re_humidity_centi_pct = sensedHumidity;

  temperature = (uint16)zb_re_temperature_centi_c;
  payload[0] = ZB_RE_CMD_REPORT;
  payload[1] = zb_re_sequence;
  payload[2] = (uint8)(temperature >> 8);
  payload[3] = (uint8)temperature;
  payload[4] = (uint8)(zb_re_humidity_centi_pct >> 8);
  payload[5] = (uint8)zb_re_humidity_centi_pct;

  if (zb_re_send(zb_re_router_address, payload, sizeof(payload)) == ZSuccess) {
    zb_re_waiting_ack = TRUE;
    osal_start_timerEx(zclGenericApp_TaskID,
                       ZB_RE_ACK_TIMEOUT_EVT,
                       ZB_RE_ACK_TIMEOUT_MS);
  }
}

static void zb_re_send_ack(uint16 destination, uint8 sequence, uint8 status)
{
  uint8 payload[3];

  payload[0] = ZB_RE_CMD_ACK;
  payload[1] = sequence;
  payload[2] = status;
  (void)zb_re_send(destination, payload, sizeof(payload));
}

void ZbRouterEndDevice_Init(void)
{
  zb_re_transaction_id = 0;
  zb_re_sequence = 0;
  zb_re_network_ready = FALSE;
  zb_re_waiting_ack = FALSE;
  zb_re_retry_count = 0;
  zb_re_router_address = ZB_RE_BROADCAST_ADDR;
  zb_re_last_report_valid = FALSE;

#if (ZB_NODE_ROLE == ZB_ROLE_END_DEVICE)
  DHT11_Init();
#endif

#if (ZB_NODE_ROLE == ZB_ROLE_ROUTER)
  osal_start_timerEx(zclGenericApp_TaskID, ZB_RE_HELLO_EVT, 1000);
#endif
}

void ZbRouterEndDevice_SetNetworkReady(uint8 ready)
{
  zb_re_network_ready = ready ? TRUE : FALSE;
  if (!zb_re_network_ready) {
    zb_re_waiting_ack = FALSE;
    zb_re_router_address = ZB_RE_BROADCAST_ADDR;
    osal_stop_timerEx(zclGenericApp_TaskID, ZB_RE_ACK_TIMEOUT_EVT);
  }
}

uint16 ZbRouterEndDevice_ProcessEvent(uint16 events)
{
#if (ZB_NODE_ROLE == ZB_ROLE_ROUTER)
  if (events & ZB_RE_HELLO_EVT) {
    zb_re_send_hello();
    osal_start_timerEx(zclGenericApp_TaskID,
                       ZB_RE_HELLO_EVT,
                       zb_re_network_ready ? ZB_RE_HELLO_PERIOD_MS : ZB_RE_HELLO_RETRY_MS);
    events ^= ZB_RE_HELLO_EVT;
  }
#else
  if (events & ZB_RE_REPORT_EVT) {
    if (!zb_re_waiting_ack) {
      zb_re_send_report();
    }
    events ^= ZB_RE_REPORT_EVT;
  }

  if (events & ZB_RE_ACK_TIMEOUT_EVT) {
    if (zb_re_waiting_ack) {
      zb_re_waiting_ack = FALSE;
      if (zb_re_retry_count < ZB_RE_MAX_RETRIES) {
        ++zb_re_retry_count;
        osal_set_event(zclGenericApp_TaskID, ZB_RE_REPORT_EVT);
      } else {
        zb_re_retry_count = 0;
        osal_start_timerEx(zclGenericApp_TaskID,
                           ZB_RE_REPORT_EVT,
                           ZB_RE_REPORT_PERIOD_MS);
      }
    }
    events ^= ZB_RE_ACK_TIMEOUT_EVT;
  }
#endif

  return events;
}

void ZbRouterEndDevice_HandleIncoming(afIncomingMSGPacket_t *packet)
{
  uint8 command;
  uint8 sequence;
  int16 temperature;
  uint16 humidity;

  if (packet == NULL || packet->clusterId != ZB_RE_CLUSTER_ID ||
      packet->cmd.Data == NULL || packet->cmd.DataLength == 0) {
    return;
  }

  command = packet->cmd.Data[0];

#if (ZB_NODE_ROLE == ZB_ROLE_ROUTER)
  if (command == ZB_RE_CMD_REPORT) {
    sequence = (packet->cmd.DataLength >= 2) ? packet->cmd.Data[1] : 0;
    if (packet->cmd.DataLength != 6) {
      zb_re_send_ack(packet->srcAddr.addr.shortAddr, sequence, ZB_RE_STATUS_BAD_LENGTH);
      return;
    }

    temperature = (int16)(((uint16)packet->cmd.Data[2] << 8) |
                           packet->cmd.Data[3]);
    humidity = (uint16)(((uint16)packet->cmd.Data[4] << 8) |
                        packet->cmd.Data[5]);
    zb_re_last_source = packet->srcAddr.addr.shortAddr;
    zb_re_last_temperature = temperature;
    zb_re_last_humidity = humidity;
    zb_re_last_sequence = sequence;
    zb_re_last_report_valid = TRUE;
    zb_re_send_ack(zb_re_last_source, sequence, ZB_RE_STATUS_OK);
  }
#else
  if (command == ZB_RE_CMD_HELLO && packet->cmd.DataLength == 3) {
    zb_re_router_address = packet->srcAddr.addr.shortAddr;
    zb_re_retry_count = 0;
    if (!zb_re_waiting_ack) {
      osal_start_timerEx(zclGenericApp_TaskID, ZB_RE_REPORT_EVT, 500);
    }
  } else if (command == ZB_RE_CMD_ACK && packet->cmd.DataLength == 3 &&
             packet->cmd.Data[1] == zb_re_sequence && zb_re_waiting_ack &&
             packet->cmd.Data[2] == ZB_RE_STATUS_OK) {
    zb_re_waiting_ack = FALSE;
    zb_re_retry_count = 0;
    ++zb_re_sequence;
    osal_stop_timerEx(zclGenericApp_TaskID, ZB_RE_ACK_TIMEOUT_EVT);
    osal_start_timerEx(zclGenericApp_TaskID,
                       ZB_RE_REPORT_EVT,
                       ZB_RE_REPORT_PERIOD_MS);
  }
#endif
}

void ZbRouterEndDevice_SetMeasurement(int16 temperatureCentiC,
                                      uint16 humidityCentiPct)
{
  zb_re_temperature_centi_c = temperatureCentiC;
  zb_re_humidity_centi_pct = humidityCentiPct;
}

uint8 ZbRouterEndDevice_GetLastReport(uint16 *sourceAddress,
                                      int16 *temperatureCentiC,
                                      uint16 *humidityCentiPct,
                                      uint8 *sequence)
{
  if (!zb_re_last_report_valid) return FALSE;
  if (sourceAddress != NULL) *sourceAddress = zb_re_last_source;
  if (temperatureCentiC != NULL) *temperatureCentiC = zb_re_last_temperature;
  if (humidityCentiPct != NULL) *humidityCentiPct = zb_re_last_humidity;
  if (sequence != NULL) *sequence = zb_re_last_sequence;
  return TRUE;
}
