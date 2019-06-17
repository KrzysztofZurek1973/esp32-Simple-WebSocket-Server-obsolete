/*
 * websocket_server.h
 *
 *  Created on: May 6, 2019
 *      Author: kz
 */

#ifndef MAIN_WEBSOCKET_SERVER_H_
#define MAIN_WEBSOCKET_SERVER_H_

#include "lwip/api.h"

typedef void *ws_handler_t;

/** \brief Opcode according to RFC 6455*/
typedef enum {
	WS_OP_CON = 0x0, 				/*!< Continuation Frame*/
	WS_OP_TXT = 0x1, 				/*!< Text Frame*/
	WS_OP_BIN = 0x2, 				/*!< Binary Frame*/
	WS_OP_CLS = 0x8, 				/*!< Connection Close Frame*/
	WS_OP_PIN = 0x9, 				/*!< Ping Frame*/
	WS_OP_PON = 0xa 				/*!< Pong Frame*/
} WS_OPCODES;

typedef struct{
	WS_OPCODES opcode:4;
	uint8_t reserved:3;
	uint8_t fin:1;
	uint8_t payload_len:7;
	uint8_t mask:1;
}ws_frame_header_t;

typedef union{
	ws_frame_header_t h;
	uint8_t bytes[2];
} ws_frame_header_u_t;

// state of websocket
typedef enum {
	WS_CLOSED =	0x0,
	WS_OPEN = 0x1,
	WS_CLOSING = 0x2,
	WS_OPENING = 0x3
} WS_STATE;

// websocket's run state
typedef enum {
	WS_STOP = 0x0,
	WS_RUN = 0x1
} WS_RUNING;

typedef struct{
	uint8_t *payload;
	uint16_t len;
	int8_t index;
	WS_OPCODES opcode:4;
	uint8_t ws_frame:1; //ws - 1, non ws - 0
	uint8_t text:1; //1 - text frame, 0 - binary frame
}ws_queue_item_t;

typedef struct{
	uint8_t *payload;
	int len;
}ws_send_data;

//configuration structure
typedef struct ws_server_cfg{
	uint16_t port;
} ws_server_cfg_t;

int8_t ws_server_init(void *param);
int8_t ws_server_stop(void);
int8_t ws_send(ws_queue_item_t *item, int32_t wait_ms);
xQueueHandle ws_get_recv_queue(void);


#endif /* MAIN_WEBSOCKET_SERVER_H_ */
