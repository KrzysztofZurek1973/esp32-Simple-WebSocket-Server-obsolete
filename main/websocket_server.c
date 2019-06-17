/*
 * websocket_server.c
 *
 *  Created on: May 6, 2019
 *      Author: Krzysztof Zurek
 *      Notes: some code is inspired by
 *      http://www.barth-dev.de/websockets-on-the-esp32/#
 */

#include <stdio.h>
#include <sys/param.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "hwcrypto/sha.h"
#include "wpa2/utils/base64.h"

#include "lwip/api.h"

#include "websocket_server.h"

#define MAX_PAYLOAD_LEN		1024
#define MAX_OPEN_WS_NR		5	//max number of opened websockets
#define SHA1_RES_LEN		20	//sha1 result length
#define CLOSE_TIMEOUT_MS	2000 //ms

struct ws_list_item{
	struct netconn *netconn_ptr;
	xTaskHandle ws_task_handl;
	TimerHandle_t ws_timer;
	uint32_t pings;
	uint32_t pongs;
	uint8_t index;
	WS_RUNING run:1;
	WS_STATE ws_state:2;
};

//global server variables
static int8_t server_is_running = 0;
static xTaskHandle server_task_handle;
struct ws_list_item ws_list[MAX_OPEN_WS_NR];
static struct netconn *server_conn;
xQueueHandle ws_output_queue;
xQueueHandle ws_input_queue;
static xSemaphoreHandle xServerMutex;
static xSemaphoreHandle xSendMutex;

//tasks functions
static void server_task(void* arg);
static void ws_receive_task(void* arg);
static void ws_send_task(void* arg);
static uint8_t head_buff[MAX_PAYLOAD_LEN + 4]; //sending buffer

//functions prototypes
void add_ws_header(ws_queue_item_t *q, ws_send_data *ws_data);
uint8_t close_ws(uint16_t error_nr, int8_t i);
int8_t ws_handshake(uint8_t *rq, uint8_t index, ws_queue_item_t *ws_item);
void vCloseTimeoutCallback(TimerHandle_t xTimer);

// This is the data from the busy server
static char error_busy_page[] =
		"HTTP/1.1 503 Service Unavailable\r\n\r\n";
//handshake strings
const char ws_sec_key[] = "Sec-WebSocket-Key";
const char ws_upgrade[] = "Upgrade: websocket";
const char ws_conn_1[] = "Connection: Upgrade";
const char ws_conn_2[] = "Connection: keep-alive, Upgrade";
const char ws_ver[] = "Sec-WebSocket-Version: 13";
const char ws_sec_conKey[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
const char ws_server_hs[] = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: "\
		"websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n";

// ****************************************************************************
//websocket task function
static void ws_receive_task(void* arg){
	struct netconn *ws_conn;
	int msg_ok = 0, msg_start = 0;
	uint16_t ws_len = 0, tcp_len = 0;
	struct netbuf *inbuf;
	uint8_t *rq, *msg;
	ws_frame_header_t *ws_header;
	uint8_t masking_key[4];
	uint8_t offset = 2, mask = 0, finish;
	WS_OPCODES opcode;
	int8_t ws_tab_index;
	err_t err, rcv_err;
	ws_queue_item_t *ws_item;

	ws_tab_index = *(int8_t *)arg;
	printf("receive task starting, index: %i\n", ws_tab_index);
	ws_conn = ws_list[ws_tab_index].netconn_ptr; //open websocket connection
	msg = NULL;
	opcode = 0;
	rcv_err = ERR_OK;
	xSemaphoreGive(xServerMutex);

	while(1){
		if (ws_list[ws_tab_index].run == WS_STOP){
			break;
		}
		rcv_err = netconn_recv(ws_conn, &inbuf);
		if (rcv_err == ERR_OK){
			msg_ok = 0;

			//read data from input buffer
			netbuf_data(inbuf, (void**) &rq, &tcp_len);

			if (ws_list[ws_tab_index].ws_state != WS_CLOSED){
				//receive websocket data from client
				offset = 0;
				if (msg_start == 0){
					//start of the message
					ws_header = (ws_frame_header_t *)rq;
					ws_len = ws_header -> payload_len;
					opcode = ws_header -> opcode;
					finish = ws_header -> fin;
					if (finish == 0){
						//fragmentation not supported
						close_ws(1007, ws_tab_index);
						break;
					}
					offset = 2;
					if (ws_len == 126){
						//message length are bytes 2 and 3
						ws_len = (rq[offset] << 8) + rq[offset + 1];
						offset = 4;
						if (ws_len > MAX_PAYLOAD_LEN){
							close_ws(1009, ws_tab_index);
							msg_ok = -1;
						}
					}
					else if (ws_len == 127){
						//64bit addresses are not supported
						close_ws(1009, ws_tab_index);
						msg_ok = -1;
					}
					//printf("received msg len = %i\n", ws_len);
					mask = ws_header -> mask;
					if (mask == 0x1){
						masking_key[0] = rq[offset];
						masking_key[1] = rq[offset + 1];
						masking_key[2] = rq[offset + 2];
						masking_key[3] = rq[offset + 3];
						offset += 4;
					}
					//allocate memory for message
					msg = malloc(ws_len + 1);
				}
				//copy data to buffer
				memcpy(msg + msg_start, rq + offset, tcp_len - offset);
				msg_start += tcp_len - offset;
				if (msg_start == ws_len){
					//all data received, unmask data
					printf("msg len ok, %i\n", ws_len);
					if (mask == 1){
						//unmask data
						for (int i = 0; i < ws_len; i++){
							msg[i] = msg[i] ^ masking_key[i%4];
						}
					}
					msg_ok = 1;
					msg[ws_len] = 0;
					msg_start = 0;
				}
				else if (msg_start > ws_len){
					//message length error, close connection
					printf("msg length error, %i, %i\n", msg_start, ws_len);
					close_ws(1011, ws_tab_index);
					free(msg);
					msg = NULL; //is it necessary?
					msg_ok = -1;
				}
			}
			netbuf_delete(inbuf);

			//collect message, check it
			switch (ws_list[ws_tab_index].ws_state){
			case WS_OPEN:
				if (msg_ok == 1){
					switch(opcode){
					case WS_OP_TXT:
					case WS_OP_BIN:
						//application data received
						//printf("app data received: %s\n", msg);
						ws_item = malloc(sizeof(ws_queue_item_t));
						ws_item -> payload = msg;
						ws_item -> len = ws_len;
						ws_item -> index = ws_tab_index;
						ws_item -> opcode = 0x0;
						ws_item -> ws_frame = 0x1;
						if (opcode == WS_OP_TXT){
							ws_item -> text = 0x1;
						}
						else{
							ws_item -> text = 0x0;
						}
						//send websocket data to application
						xQueueSend(ws_input_queue, &ws_item, portMAX_DELAY);
						break;
					case WS_OP_CLS:
						//close connection
						printf("close connection, index = %i\n", ws_tab_index);
						close_ws((msg[0] << 8) + msg[1], ws_tab_index);
						free(msg);
						msg = NULL;
						break;
					case WS_OP_PIN:
						//ping control frame
						ws_item = malloc(sizeof(ws_queue_item_t));
						ws_item -> payload = msg;
						ws_item -> len = ws_len;
						ws_item -> index = ws_tab_index;
						ws_item -> opcode = WS_OP_PON;
						ws_item -> ws_frame = 0x1;
						ws_item -> text = 0x0;
						//increment ping number
						ws_list[ws_tab_index].pings++;
						//printf("ping received, %i\n", ws_list[ws_tab_index].pings);
						//send pong
						xQueueSend(ws_output_queue, ws_item, portMAX_DELAY);
						break;
					case WS_OP_PON:
						ws_list[ws_tab_index].pongs++;
						//printf("pong received, %i\n", ws_list[ws_tab_index].pongs);
						break;
					case WS_OP_CON:
					default:
						//TODO: what to do if happen?
						printf("incorrect opcode received: %X\n", opcode);
						close_ws(1008, ws_tab_index);
						free(msg);
						break;
					}
				}
				break;

			case WS_CLOSED:
				//check if request was http 'GET /\r\n'
				if(rq[0] == 'G' && rq[1] == 'E' && rq[2] == 'T'
						&& rq[3] == ' ' && rq[4] == '/') {
					ws_item = malloc(sizeof(ws_queue_item_t));
					//printf("hs, ws_item addr = %p\n", ws_item);
					if (ws_item != NULL){
						uint8_t res = ws_handshake(rq, ws_tab_index, ws_item);
						if (res == 1){
							xQueueSendToFront(ws_output_queue, &ws_item, portMAX_DELAY);
						}
						else{
							printf("ws_handshake returned error\n");
						}
					}
					else{
						printf("handshake, no heap memory\n");
					}
				}
				else{
					//wrong, open request, close connection
					ws_list[ws_tab_index].run = WS_STOP;
					printf("ERROR: bad http request at handshake\n");
				}
				break;
			case WS_OPENING:
				//should not happen
				printf("ws state is OPENING, received opcode = %X, msg = %s\n", opcode, msg);
				break;
			case WS_CLOSING:
				if (opcode == WS_OP_CLS){
					printf("client answer on close frame, close code = %i", (msg[0] << 8) + msg[1]);
					ws_list[ws_tab_index].run = WS_STOP;
					//TODO: if this is not answer for server's CLOSE, but client's fist
					//CLOSE frame, then client is waiting for server's CLOSE
				}
				else{
					printf("state CLOSING, incorrect ws frame, opcode = %X\n", opcode);
				}
				//ignore other opcodes
				break;
			default:
				ws_list[ws_tab_index].run = WS_STOP;
			}//switch(ws_state)
		} //netconn_recv
		else{
			if (rcv_err == -15){
				printf("TCP was closed by client\n");
			}
			else{
				printf("Incorrect data received, index: %i, error = %i\n", ws_tab_index, rcv_err);
			}
			ws_list[ws_tab_index].run = WS_STOP;
		}
	} //while

	printf("receive task is going down, index = %i\n", ws_tab_index);

	//close TCP connection
	if (rcv_err != ERR_CLSD){
		err = netconn_close(ws_conn);
		if (err != ERR_OK){
			printf("Receive task, recv error, connection can't be closed, error %i\n", err);
		}
	}

	//stop time-out timer
	if (ws_list[ws_tab_index].ws_timer != NULL){
		xTimerStop(ws_list[ws_tab_index].ws_timer, 0);
		xTimerDelete(ws_list[ws_tab_index].ws_timer, 0);
		ws_list[ws_tab_index].ws_timer = NULL;
	}

	ws_list[ws_tab_index].netconn_ptr = NULL;
	ws_list[ws_tab_index].ws_state = WS_CLOSED;
	ws_list[ws_tab_index].run = WS_STOP;
	//delete websocket task
	vTaskDelete(NULL);
}


// ***************************************************************************
int8_t ws_handshake(uint8_t *rq, uint8_t index, ws_queue_item_t *ws_item){
	uint8_t msg_flags = 0;
	int8_t ret;
	char *buff_1, *buff_2, *buff_3, *server_ans;
	char *res1, *res2;

	server_ans = NULL;

	//upgrade
	if (strstr((char *)rq, ws_upgrade)){
		msg_flags |= 0x01;
	}
	//connection
	if (strstr((char *)rq, ws_conn_1)){
		msg_flags |= 0x02;
	}
	else if (strstr((char *)rq, ws_conn_2)){
		msg_flags |= 0x02;
	}
	//ver
	if (strstr((char *)rq, ws_ver)){
		msg_flags |= 0x04;
	}
	if (msg_flags == 0x07){
		size_t  out_len;

		res1 = strstr((char *)rq, ws_sec_key);
		if (res1 != NULL){
			msg_flags |= 0x08;

			res2 = strstr(res1, ": ");
			res1 = strstr(res2, "\r\n");
			buff_1 = malloc(80);
			memset(buff_1, 0, 80);
			memcpy(buff_1, res2 + 2, res1 - res2 - 2);

			//concatenate websocket GUID
			strcpy((char *)&buff_1[res1 - res2 - 2], ws_sec_conKey);
			buff_1[res1 - res2 - 2 + strlen(ws_sec_conKey)] = 0;

			buff_2 = malloc(20);
			esp_sha(SHA1, (unsigned char*)buff_1, strlen(buff_1), (unsigned char*)buff_2);
			free(buff_1);

			out_len = 4 + SHA1_RES_LEN*4/3 + 1;
			buff_3 = (char*)base64_encode((unsigned char*)buff_2, SHA1_RES_LEN, (size_t*)&out_len);
			free(buff_2);

			//prepare server answer
			server_ans = malloc(out_len + strlen(ws_server_hs) + 10);
			sprintf(server_ans, ws_server_hs, buff_3);
			//printf("%s\n", server_ans);
			free(buff_3);
		}
	}

	//send answer to the client
	if ((msg_flags == 0x0F) & (server_ans != NULL)){

		ws_list[index].ws_state = WS_OPENING;

		ws_item -> payload = (uint8_t *)server_ans;
		ws_item -> len = strlen(server_ans);
		ws_item -> opcode = 0;
		ws_item -> ws_frame = 0;
		ws_item -> index = index;
		//printf("data length = %i\n", ws_item -> len);
		ret = 1;
	}
	else{
		ret = -1;
		printf("ws_handshake error, msg_flags = %X\n", msg_flags);
	}
	return ret;
}

// ****************************************************************************
//close websocket
uint8_t close_ws(uint16_t error_nr, int8_t ws_tab_index){
	char *payload;
	ws_queue_item_t *ws_item;
	TimerHandle_t timeout_timer;

	printf("connection will be closed, i = %i\n", ws_tab_index);

	//prepare close frame with close code
	payload = malloc(2);
	payload[0] = error_nr >> 8;
	payload[1] = error_nr;

	ws_item = malloc(sizeof(ws_queue_item_t));
	ws_item -> payload = (uint8_t *)payload;
	ws_item -> len = 2;
	ws_item -> index = ws_tab_index;
	ws_item -> opcode = WS_OP_CLS; //close
	ws_item -> ws_frame = 0x1;
	xQueueSend(ws_output_queue, &ws_item, portMAX_DELAY);

	//create time-out timer
	timeout_timer = xTimerCreate("timeout", pdMS_TO_TICKS(CLOSE_TIMEOUT_MS),
			pdFALSE, (void *)&ws_list[ws_tab_index].index,
			vCloseTimeoutCallback);

	ws_list[ws_tab_index].ws_timer = timeout_timer;
	if (ws_list[ws_tab_index].ws_state == WS_OPEN){
		ws_list[ws_tab_index].ws_state = WS_CLOSING;
	}
	else{
		ws_list[ws_tab_index].ws_state = WS_CLOSED;
		ws_list[ws_tab_index].netconn_ptr = NULL;
		printf("conn closed, index = %i\n", ws_tab_index);
	}
	//start timer
	xTimerStart(timeout_timer, 0);

	return 1;
}

// ****************************************************************************
//closing timer callback
void vCloseTimeoutCallback( TimerHandle_t xTimer ){
	uint8_t index;
	struct netconn *conn;
	err_t err;

	index = *(uint8_t *) pvTimerGetTimerID(xTimer);

	conn = ws_list[index].netconn_ptr;
	printf("timeout, index = %i\n", index);
	if (conn != NULL){
		ws_list[index].run = WS_STOP;
		err = netconn_close(conn);
		if (err != ERR_OK){
			//TODO: what if can't be closed?
			printf("Timeout callback, connection can't be closed, error %i\n", err);
		}
		else{
			printf("conn closed in timer\n");
		}
	}
}

// ****************************************************************************
//prepare websocket header and send data to client
static void ws_send_task(void* arg){
	ws_queue_item_t *q_item;
	ws_send_data ws_data;
	int8_t index, data_sent;

	for(;;){
		xQueueReceive(ws_output_queue, &q_item, portMAX_DELAY);

		data_sent = 0;
		memset(head_buff, 0, q_item -> len + 4);
		if (q_item -> ws_frame == 0x1){
			add_ws_header(q_item, &ws_data);
		}
		else{
			memcpy(head_buff, q_item -> payload, q_item -> len);
			ws_data.payload = head_buff;
			ws_data.len = q_item -> len;
		}
		index = q_item -> index;
		free(q_item -> payload);

		//send data to one or all clients
		if (ws_data.payload != NULL){
			if (index == -1){
				//send to all clients
				for (int i = 0; i < MAX_OPEN_WS_NR; i++){
					if (ws_list[i].ws_state == WS_OPEN){
						err_t err = netconn_write(ws_list[i].netconn_ptr,
								ws_data.payload,
								ws_data.len,
								NETCONN_COPY);
						if (err != ERR_OK){
							printf("data not sent to multi, index = %i, err = %i, \ndata: %s\n",
									i, err, (char *)ws_data.payload);
						}
						else{
							data_sent++;
						}
					}
				}
			}
			else{
				//send to only one given client
				WS_STATE state;

				state = ws_list[index].ws_state;
				if ((state == WS_OPEN) || (state == WS_OPENING) || (state == WS_CLOSING)){
					err_t err = netconn_write(ws_list[index].netconn_ptr,
							ws_data.payload,
							ws_data.len,
							NETCONN_COPY);
					if (err != ERR_OK){
						//TODO: what if answer for open handshake was not sent?
						printf("data not sent to one, index = %i, err = %i, \ndata:%s\n",
								index, err, (char *)ws_data.payload);
					}
					else{
						if (ws_list[index].ws_state == WS_OPENING){
							ws_list[index].ws_state = WS_OPEN;
						}
						data_sent++;
					}
				}
				else{
					printf("ERROR: single, websocket incorrect state\n");
				}
			}
			if (data_sent > 0){
				//for test
				/*
				if (q_item -> ws_frame == 0x1){
					if (ws_data.len <= 125){
						printf("header: 0x%X 0x%X\n", ws_data.payload[0], ws_data.payload[1]);
						printf("server data for client: %s\n", &ws_data.payload[2]);
					}
					else{
						printf("header: 0x%X 0x%X 0x%X 0x%X\n", ws_data.payload[0],
								ws_data.payload[1], ws_data.payload[2], ws_data.payload[3]);
						printf("server data for client: %s\n", &ws_data.payload[4]);
					}
				}
				else{
					printf("server data for client: %s\n", ws_data.payload);
				}
				*/
				//end of "for test"
			}
		}
		else{
			printf("ws_send, no heap memory\n");
		}
		free(q_item);
	} //for
}

// ****************************************************************************
//add websocket header to sending data
void add_ws_header(ws_queue_item_t *q, ws_send_data *out){
	ws_frame_header_u_t header;

	out -> payload = head_buff;
	//add websocket header
	header.h.opcode = q -> opcode;
	header.h.fin = 0x1;
	header.h.mask = 0x0;	//only client masks data
	if (q -> len <= 125){
		header.h.payload_len = q -> len;
		head_buff[0] = header.bytes[0];
		head_buff[1] = header.bytes[1];
		memcpy(head_buff + 2, q -> payload, q -> len);
		out -> len = q -> len + 2;
	}
	else if (q -> len <= MAX_PAYLOAD_LEN){
		header.h.payload_len = 126;
		head_buff[0] = header.bytes[0];
		head_buff[1] = header.bytes[1];
		head_buff[2] = (q -> len) >> 8;
		head_buff[3] = (q -> len) & 0x00FF;
		memcpy(head_buff + 4, q -> payload, q -> len);
		out -> len = q -> len + 4;
	}
	else{
		//too long message
		out -> len = 0;
		out -> payload = NULL;
	}
}


// ***************************************************************************
//initialize WebSocket server
int8_t ws_server_init(void *param){
	int8_t ret;
	ws_queue_item_t *item_ptr;

	//ws_server_handler = NULL;
	xServerMutex = xSemaphoreCreateMutex();
	xSendMutex = xSemaphoreCreateMutex();
	//initialize ws_list
	for (int i = 0; i < MAX_OPEN_WS_NR; i++){
		ws_list[i].netconn_ptr = NULL;
		ws_list[i].ws_timer = NULL;
		ws_list[i].index = i;
		ws_list[i].pings = 0;
		ws_list[i].run = WS_STOP;
		ws_list[i].ws_state = WS_CLOSED;
	}
	//start server task
	if (server_is_running == 0){
		//printf("ws server init, q item size: %i\n", sizeof(item_ptr));
		vTaskDelay(1000 / portTICK_PERIOD_MS);
		ws_output_queue = xQueueCreate(10, sizeof(item_ptr));
		if (ws_output_queue != NULL){
			printf("OUT queue created\n");
		}
		else{
			printf("OUT queue not created\n");
		}
		//open input (receiving) queue
		ws_input_queue = xQueueCreate(10, sizeof(item_ptr));
		printf("IN queue created\n");

		if ((ws_output_queue != NULL) && (ws_input_queue != NULL)){
			xTaskCreate(server_task, "ws_server_task", 1024*4, param, 3, &server_task_handle);
			printf("server task created\n");
			server_is_running = 1;
		}
		else{
			printf("ws server not created\n");
		}
		//open output (sending) queue
		xTaskCreate(ws_send_task, "ws_send_task", 2048, NULL, 1, NULL);
		ret = 1;
	}
	else{
		ret = -1;
	}
	return ret;
}

// ****************************************************************************
//send data via websocket
int8_t ws_send(ws_queue_item_t *item, int32_t wait_ms){

	return xQueueSend(ws_output_queue, &item, wait_ms / portTICK_RATE_MS);
}

// ****************************************************************************
int8_t ws_server_stop(){

	//close server connection
	netconn_close(server_conn);
	vTaskDelete(server_task_handle);
	server_is_running = 0;

	//TODO: close all connection and stop all websocket tasks
	return 1;
}

// ****************************************************************************
//main server function
void server_task(void* arg){
	ws_server_cfg_t *cfg;
	uint16_t port;
	struct netconn *newconn;
	int8_t index;

	cfg = (ws_server_cfg_t *)arg;
	port = cfg -> port;

	//set up new TCP listener
	server_conn = netconn_new(NETCONN_TCP);
	netconn_bind(server_conn, NULL, port);
	netconn_listen(server_conn);
	printf("WebSocket server in listening mode\n");
	vTaskDelay(1000 / portTICK_PERIOD_MS);

	for (;;){
		if (netconn_accept(server_conn, &newconn) == ERR_OK){
			//check if there is place for next client
			xSemaphoreTake(xServerMutex, portMAX_DELAY);
			index = -1;
			printf("new client connected\n");
			for (int i = 0; i < MAX_OPEN_WS_NR; i++){
				if (ws_list[i].netconn_ptr == NULL){
					index = i;
					break;
				}
			}
			if (index > -1){
				printf("client will be served, index: %i\n", index);
				ws_list[index].netconn_ptr = newconn;
				ws_list[index].ws_state = WS_CLOSED;
				ws_list[index].ws_timer = NULL;
				ws_list[index].index = index;
				ws_list[index].pings = 0;
				ws_list[index].pongs = 0;
				ws_list[index].run = WS_RUN;

				xTaskCreate(ws_receive_task, "ws_task", 1024*2, &index, 3,
						&ws_list[index].ws_task_handl);
			}
			else{
				//too much clients, send error info and close connection
				//TODO: there was no http request, is it correct to send data now?
				xSemaphoreGive(xServerMutex);
				printf("no space for new clients\n");
				netconn_write(newconn, error_busy_page, sizeof(error_busy_page), NETCONN_COPY);
				netconn_close(newconn);
			}
		}
	}
}


// ****************************************************************************
xQueueHandle ws_get_recv_queue(){
	return ws_input_queue;
}
