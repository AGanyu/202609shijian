#ifndef ZB_ROUTER_END_DEVICE_H
#define ZB_ROUTER_END_DEVICE_H

#include "ZComDef.h"
#include "AF.h"

#define ZB_ROLE_ROUTER                 1
#define ZB_ROLE_END_DEVICE             2

#ifndef ZB_NODE_ROLE
#define ZB_NODE_ROLE                   ZB_ROLE_ROUTER
#endif

#define ZB_RE_CLUSTER_ID               0xFC10
#define ZB_RE_ENDPOINT                 8

#define ZB_RE_HELLO_EVT                0x0040
#define ZB_RE_REPORT_EVT               0x0080
#define ZB_RE_ACK_TIMEOUT_EVT          0x0100

#define ZB_RE_CMD_HELLO                0x01
#define ZB_RE_CMD_REPORT               0x02
#define ZB_RE_CMD_ACK                  0x03

#define ZB_RE_STATUS_OK                0x00
#define ZB_RE_STATUS_BAD_LENGTH        0x01
#define ZB_RE_STATUS_BAD_COMMAND       0x02

void ZbRouterEndDevice_Init(void);
void ZbRouterEndDevice_SetNetworkReady(uint8 ready);
uint16 ZbRouterEndDevice_ProcessEvent(uint16 events);
void ZbRouterEndDevice_HandleIncoming(afIncomingMSGPacket_t *packet);

void ZbRouterEndDevice_SetMeasurement(int16 temperatureCentiC,
                                      uint16 humidityCentiPct);
uint8 ZbRouterEndDevice_GetLastReport(uint16 *sourceAddress,
                                      int16 *temperatureCentiC,
                                      uint16 *humidityCentiPct,
                                      uint8 *sequence);

#endif /* ZB_ROUTER_END_DEVICE_H */
