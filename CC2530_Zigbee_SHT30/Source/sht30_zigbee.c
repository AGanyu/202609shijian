#include "ZComDef.h"
#include "OSAL.h"
#include "AF.h"
#include "onboard.h"
#include "zcl_genericapp.h"
#include "sht30.h"
#include "sht30_zigbee.h"

#define SHT30_ZIGBEE_CLUSTER_ID       0xFC00
#define SHT30_ZIGBEE_DEST_ADDR        0x0000
#define SHT30_ZIGBEE_DEST_ENDPOINT    GENERICAPP_ENDPOINT
#define SHT30_ZIGBEE_PERIOD_MS        30000

static uint8 sht30_transaction_id;
static endPointDesc_t sht30_source_endpoint =
{
  GENERICAPP_ENDPOINT,
  0,
  &zclGenericApp_TaskID,
  &zclGenericApp_SimpleDesc,
  noLatencyReqs
};

void SHT30_ZigbeeInit(void)
{
  sht30_transaction_id = 0;
  SHT30_Init();
  osal_start_timerEx(zclGenericApp_TaskID, SHT30_MEASURE_EVT, 2000);
}

void SHT30_ZigbeeSampleAndSend(void)
{
  afAddrType_t destination;
  uint8 payload[7];
  uint8 status;
  uint8 sendStatus;
  int16 temperature;
  uint16 humidity;

  status = SHT30_ReadMeasurement(&temperature, &humidity);
  payload[0] = 0x01; /* Payload version. */
  payload[1] = status;
  payload[2] = (uint8)(((uint16)temperature >> 8) & 0xFF);
  payload[3] = (uint8)((uint16)temperature & 0xFF);
  payload[4] = (uint8)(humidity >> 8);
  payload[5] = (uint8)(humidity & 0xFF);
  payload[6] = sht30_transaction_id++;

  destination.addrMode = afAddr16Bit;
  destination.addr.shortAddr = SHT30_ZIGBEE_DEST_ADDR;
  destination.endPoint = SHT30_ZIGBEE_DEST_ENDPOINT;
  destination.panId = 0;

  sendStatus = AF_DataRequest(&destination,
                              &sht30_source_endpoint,
                              SHT30_ZIGBEE_CLUSTER_ID,
                              sizeof(payload),
                              payload,
                              &sht30_transaction_id,
                              AF_ACK_REQUEST,
                              AF_DEFAULT_RADIUS);
  (void)sendStatus;

  osal_start_timerEx(zclGenericApp_TaskID,
                     SHT30_MEASURE_EVT,
                     SHT30_ZIGBEE_PERIOD_MS);
}
