# esp32-Simple-WebSocket-Server
WebSocket server for esp32 IDF environment. It uses freeRTOS and LwIP.

## Introduction
This is an implementation of WebSocket server with example application. It serves only WebSocket messages, this is not HTTP server.

This code does not support:
- messages bigger then 65 kB,
- fragmented messages,
- WebSocket tunneled over TLS.

It is written and tested in the ESP-IDF environment, using the xtensa-esp32-elf toolchain, on ESP32-DevKitC V4 with ESP32-WROOM-32 module.

The easiest way to work with the code is to use Eclipse.
Eclipse configuration is available on: [here](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/eclipse-setup.html)

## This example provides
1. wifi connection configuration, fill in `ESP_WIFI_SSID` with the name of your network SSID and `ESP_WIFI_PASS` with your network password,
2. maximum number of open websockets is set on 5 (`MAX_OPEN_WS_NR` in `websocket_server.c`),
3. mDNS configuration, current hostname is defined in `MDNS_HOSTNAME`,
4. sending incremented number every 5 seconds to the client,
5. example www page for testing the server.

## Source Code
### To start server use the following code after receiving IP address:
(see example code in `simple_websocket_server.c`)
```
ws_server_cfg_t ws_cfg = {.port = 8080};
ws_server_init(&ws_cfg);
recv_queue = ws_get_recv_queue();	//get receing queue
xTaskCreate(ws_recv_task, "ws_recv_task", 4096, NULL, 1, NULL);
ws_server_started = 1;
```

`ws_recv_task` is the freeRTOS task which will receive messages form WebSocket, provide as much stack as will be needed (4096 bytes in this example).

`recv_queue` is the queue from which messages are retrieved in application.

### Messages should be received in the following code:
(see the example code)
```
xQueueReceive(recv_queue, &ws_queue_item, portMAX_DELAY);
msg = (char *)ws_queue_item -> payload;
```
Where `msg` is the received message buffer.
After message processing free message buffers:
```
free(ws_queue_item -> payload);
free(ws_queue_item);
```
### To send messages
**prepare `ws_queue_item_t` structure with following fields:**
* `payload` message address,
* `len` length of the message,
* `index` should be „-1” to send the message to all clients,
* `opcode` WS_OP_TXT for text messages, WS_OP_BIN for binary messages,
* `ws_frame` should be „1”.
  
**send by `ws_send(data, wait_ms)`, where:**
* `data`: prepared `ws_queue_item_t` structure,
* `wait_ms`: time to wait for space in sending queue in miliseconds.

## Source
The source is available from GitHub.
[source code](https://github.com/KrzysztofZurek1973/esp32-Simple-WebSocket-Server)

## License
The code in this project is licensed under the GPL-3.0 license - see LICENSE for details.

## Links
[Espressif IoT Development Framework for ESP32](https://github.com/espressif/esp-idf.gif)

RFC6455, [The WebSocket Protocol](https://tools.ietf.org/html/rfc6455)
