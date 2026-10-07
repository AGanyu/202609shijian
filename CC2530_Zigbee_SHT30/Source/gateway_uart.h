#ifndef GATEWAY_UART_H
#define GATEWAY_UART_H

#include "AF.h"

#define GATEWAY_UART_RX_EVT 0x0020

/* UART bridge used by the CC2530 coordinator build. */
void GatewayUartInit(void);
void GatewayUartPoll(void);
void GatewayUartHandleIncoming(afIncomingMSGPacket_t *packet);

#endif /* GATEWAY_UART_H */
