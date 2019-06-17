/*
 * Simple webSocket Server Example
 */
#include <stdio.h>
#include <sys/param.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <esp_http_server.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_spi_flash.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "driver/spi_master.h"
#include "esp_adc_cal.h"
#include "esp_attr.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "simple_websocket_server.h"
#include "websocket_server.h"

//wifi configuration data
#define ESP_WIFI_SSID      "wifi_name"
#define ESP_WIFI_PASS      "wifi_password"
#define ESP_MAXIMUM_RETRY  5
//mDNS
#define MDNS_INSTANCE "esp32-device"

//global variables
static uint8_t ws_server_started = 0;
//wifi data
/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;
static const char *TAG_WIFI = "wifi station";
static int s_retry_num = 0;
//mDNS
const int IP4_CONNECTED_BIT = BIT0;
const int IP6_CONNECTED_BIT = BIT1;
static const char *TAG_MDNS = "mdns";

static void chipInfo(void);
//network functions
static esp_err_t event_handler(void *ctx, system_event_t *event);
void wifi_init_sta(void *);
static void initialise_mdns(void);

//tasks functions
static void ws_recv_task(void* arg);
xQueueHandle recv_queue;


//***************************************************************
void app_main(){
	uint32_t i = 0;
	static httpd_handle_t http_server = NULL;
	char ws_msg[] = "{\"type\":\"message\",\"data\":{\"sensor\":\"counter\""\
					",\"value\":%i}}";
	char *msg;
	ws_queue_item_t *q_item;

	//chip information
	chipInfo();

	//Initialize NVS
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	//initialize mDNS service
	initialise_mdns();

	//initialize wifi
	ESP_LOGI(TAG_WIFI, "ESP_WIFI_MODE_STA");
	wifi_init_sta(&http_server);

	//start here additional non-network tasks

	vTaskDelay(5000 / portTICK_PERIOD_MS);
	for (;;) {
		i++;
		vTaskDelay(5000 / portTICK_PERIOD_MS);

		//prepare json message and send it
		if (ws_server_started == 1){
			msg = malloc(strlen(ws_msg) + 11);
			if (msg != NULL){
				sprintf(msg, ws_msg, i);
				q_item = malloc(sizeof(ws_queue_item_t));
				if (q_item != NULL){
					q_item -> payload = (uint8_t *)msg;
					q_item -> len = strlen(msg);
					q_item -> index = -1;
					q_item -> opcode = WS_OP_TXT;
					q_item -> ws_frame = 1;
					ws_send(q_item, 0); //send message to client
				}
			}
		}
	}

	printf("Restarting now.\n");
	fflush(stdout);
	esp_restart();
}

// *****************************************************
// webSocket receive task
static void ws_recv_task(void* arg){
	ws_queue_item_t *ws_queue_item;
	char *msg;

	for(;;){
		//wait for data from websocket
		xQueueReceive(recv_queue, &ws_queue_item, portMAX_DELAY);
		msg = (char *)ws_queue_item -> payload;

		//process received message here
		printf("%s\n", msg);

		//free buffers
		free(ws_queue_item -> payload);
		free(ws_queue_item);
	}
}


// *******************************************************
//wifi event handler
static esp_err_t event_handler(void *ctx, system_event_t *event){

	switch(event->event_id) {
    	case SYSTEM_EVENT_STA_START:
    		//ready to connect with AP
    		esp_wifi_connect();
    		break;

    	case SYSTEM_EVENT_STA_CONNECTED:
    		/* enable ipv6 */
    	    tcpip_adapter_create_ip6_linklocal(TCPIP_ADAPTER_IF_STA);
    	    break;

    	case SYSTEM_EVENT_STA_GOT_IP:
    		ESP_LOGI(TAG_WIFI, "got ip:%s",
                 ip4addr_ntoa(&event->event_info.got_ip.ip_info.ip));
    		s_retry_num = 0;
    		xEventGroupSetBits(wifi_event_group, IP4_CONNECTED_BIT);
    		//initialize websocket server
    		ws_server_cfg_t ws_cfg = {.port = 8080};
    		ws_server_init(&ws_cfg);
    		recv_queue = ws_get_recv_queue();
    		printf("websocket server started\n");
    		xTaskCreate(ws_recv_task, "ws_recv_task", 2048*2, NULL, 1, NULL);
    		ws_server_started = 1;
    		break;

    	case SYSTEM_EVENT_AP_STA_GOT_IP6:
    	    xEventGroupSetBits(wifi_event_group, IP6_CONNECTED_BIT);
    	    break;

    	case SYSTEM_EVENT_STA_DISCONNECTED:
            if (s_retry_num < ESP_MAXIMUM_RETRY) {
                esp_wifi_connect();
                xEventGroupClearBits(wifi_event_group,
                		IP4_CONNECTED_BIT | IP6_CONNECTED_BIT);
                s_retry_num++;
                ESP_LOGI(TAG_WIFI,"retry to connect to the AP");
            }
            ESP_LOGI(TAG_WIFI,"connect to the AP fail\n");
            break;

    	default:
    		break;
    }
	mdns_handle_system_event(ctx, event);
    return ESP_OK;
}

// *********************************************
//mDNS initialization
static void initialise_mdns(void)
{
    char *hostname = "esp32-ws";
    //initialize mDNS
    ESP_ERROR_CHECK( mdns_init() );
    //set mDNS hostname (required if you want to advertise services)
    ESP_ERROR_CHECK( mdns_hostname_set(hostname) );
    ESP_LOGI(TAG_MDNS, "mdns hostname set to: [%s]", hostname);
    //set default mDNS instance name
    ESP_ERROR_CHECK( mdns_instance_name_set(MDNS_INSTANCE) );

    //structure with TXT records
    mdns_txt_item_t serviceTxtData[3] = {
        {"board","esp32"},
        {"port","80"},
        {"path","/"}
    };

    //initialize service
    //port 80 is provided for additional http server not implemented here
    ESP_ERROR_CHECK( mdns_service_add("webthing", "_http", "_tcp", 80, serviceTxtData, 3) );
}


// *****************************************
//wifi initialization
void wifi_init_sta(void *arg){
    wifi_event_group = xEventGroupCreate();

    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, arg) );

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = ESP_WIFI_SSID,
            .password = ESP_WIFI_PASS
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG_WIFI, "wifi_init_sta finished.");
    ESP_LOGI(TAG_WIFI, "connect to ap SSID:%s password:%s",
    		ESP_WIFI_SSID, ESP_WIFI_PASS);
    //turn off power savings
    esp_wifi_set_ps(WIFI_PS_NONE);
}


// ************************************************
static void chipInfo(void){
	/* Print chip information */
	esp_chip_info_t chip_info;
	esp_chip_info(&chip_info);
	printf("This is ESP32 chip with %d CPU cores, WiFi%s%s, ",
			chip_info.cores,
			(chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
					(chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");

	printf("silicon revision %d, ", chip_info.revision);

	printf("%dMB %s flash\n", spi_flash_get_chip_size() / (1024 * 1024),
			(chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
}
