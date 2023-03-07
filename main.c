// Simple HTTP + SSL Server Example + ESPTOUCH

/*---------------------------------------------------------------
        LIBRARY
---------------------------------------------------------------*/

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include "esp_netif.h"
#include "esp_eth.h"
#include "protocol_examples_common.h"
#include <esp_https_server.h>
#include "esp_tls.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wpa2.h"
#include "esp_event.h"
#include "esp_smartconfig.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
/*---------------------------------------------------------------
        General Macros
---------------------------------------------------------------*/


#define led1 4
#define EXAMPLE_ADC1_CHAN0          ADC_CHANNEL_6

static int adc_raw[2][10];


int8_t led_1_state = 0;
int8_t smartcnfg_complet = 0;

double adc_1_value=0.0;

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t s_wifi_event_group;

static const char *TAG = "main";
const static char *TAG1 = "ADC";


/*Smartcnfg*/
static const int CONNECTED_BIT = BIT0;
static const int ESPTOUCH_DONE_BIT = BIT1;

//ADC1 General
adc_oneshot_unit_handle_t adc1_handle;

static esp_err_t refreshAdc();

static void smartconfig_example_task(void * parm);

esp_err_t init_led(void);
//esp_err_t toggle_led(int led);

/*---------------------------------------------------------------
        SMARTCONFIG
---------------------------------------------------------------*/
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        xTaskCreate(smartconfig_example_task, "smartconfig_example_task", 4096, NULL, 3, NULL);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, CONNECTED_BIT);
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_SCAN_DONE) {
        ESP_LOGI(TAG, "Scan done");
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_FOUND_CHANNEL) {
        ESP_LOGI(TAG, "Found channel");
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_GOT_SSID_PSWD) {
        ESP_LOGI(TAG, "Got SSID and password");

        smartconfig_event_got_ssid_pswd_t *evt = (smartconfig_event_got_ssid_pswd_t *)event_data;
        wifi_config_t wifi_config;
        uint8_t ssid[33] = { 0 };
        uint8_t password[65] = { 0 };
        uint8_t rvd_data[33] = { 0 };

        bzero(&wifi_config, sizeof(wifi_config_t));
        memcpy(wifi_config.sta.ssid, evt->ssid, sizeof(wifi_config.sta.ssid));
        memcpy(wifi_config.sta.password, evt->password, sizeof(wifi_config.sta.password));
        wifi_config.sta.bssid_set = evt->bssid_set;
        if (wifi_config.sta.bssid_set == true) {
            memcpy(wifi_config.sta.bssid, evt->bssid, sizeof(wifi_config.sta.bssid));
        }

        memcpy(ssid, evt->ssid, sizeof(evt->ssid));
        memcpy(password, evt->password, sizeof(evt->password));
        ESP_LOGI(TAG, "SSID:%s", ssid);
        ESP_LOGI(TAG, "PASSWORD:%s", password);
        if (evt->type == SC_TYPE_ESPTOUCH_V2) {
            ESP_ERROR_CHECK( esp_smartconfig_get_rvd_data(rvd_data, sizeof(rvd_data)) );
            ESP_LOGI(TAG, "RVD_DATA:");
            for (int i=0; i<33; i++) {
                printf("%02x ", rvd_data[i]);
            }
            printf("\n");
        }

        ESP_ERROR_CHECK( esp_wifi_disconnect() );
        ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
        esp_wifi_connect();
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_SEND_ACK_DONE) {
        xEventGroupSetBits(s_wifi_event_group, ESPTOUCH_DONE_BIT);
    }
}



static void smartconfig_example_task(void * parm)
{  
    EventBits_t uxBits;
    ESP_ERROR_CHECK( esp_smartconfig_set_type(SC_TYPE_ESPTOUCH) );
    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_smartconfig_start(&cfg) );
    while (smartcnfg_complet !=1 ) {
        uxBits = xEventGroupWaitBits(s_wifi_event_group, CONNECTED_BIT | ESPTOUCH_DONE_BIT, true, false, portMAX_DELAY);
        if(uxBits & CONNECTED_BIT) {
            ESP_LOGI(TAG, "WiFi Connected to ap");
        }
        if(uxBits & ESPTOUCH_DONE_BIT) {
            smartcnfg_complet=1;
            ESP_LOGI(TAG, "smartconfig over %d", smartcnfg_complet);
            esp_smartconfig_stop();   
            vTaskDelete(NULL);
        }
       
    }
}


/*---------------------------------------------------------------
        HTTPS SERVER
---------------------------------------------------------------*/
// HTTP GET handler 
static esp_err_t root_get_handler(httpd_req_t *req)
{
    /*LLamada al archivo html a partir del parámetro request*/
    extern unsigned char view_start[] asm("_binary_view_html_start");
    extern unsigned char view_end[] asm("_binary_view_html_end");
    /*Nomenclatura para obtener los binarios, obtenerlos desde memoria y servirlos en web*/
    size_t view_len = view_end - view_start;
    char viewHtml[view_len];
    /*Para obtener los datos del html desde memoria y así poder servirlos desde el servidor web*/
    memcpy(viewHtml, view_start, view_len);
    ESP_LOGI(TAG, "URI: %s", req->uri);


    //ADC
    refreshAdc();
    //adc_1_value = adc1_get_raw(ADC1_CHANNEL_6)/1241.0;

    ESP_LOGI(TAG1, "adc_1_value %1.2f", adc_1_value);
    
    //Para sustituir el %s por valores de nuestra variable
    char *viewHtmlUpdated; 
 
    double formattedStrResult = asprintf(&viewHtmlUpdated, viewHtml,led_1_state ? : "ON", led_1_state ? : "OFF", adc_1_value, adc_1_value);

    httpd_resp_set_type(req, "text/html");

    //Respuesta LED
    if (strcmp(req->uri, "/?led-On") == 0)
    { 
        gpio_set_level(led1, 1);
        //toggle_led(led1);
    }
    if (strcmp(req->uri, "/?led-Off") == 0)
    { 
        gpio_set_level(led1, 0);
        //toggle_led(led1);
    }
    if (formattedStrResult > 0)
    {
        httpd_resp_send(req, viewHtmlUpdated, view_len);
        free(viewHtmlUpdated);
    }
    else
    {
        ESP_LOGE(TAG, "Error updating variables");
        httpd_resp_send(req, viewHtml, view_len);
    }
 
    return ESP_OK;

}


static const httpd_uri_t root = {
    .uri       = "/", /*Pagina web principal*/
    .method    = HTTP_GET,
    .handler   = root_get_handler /*Método que se llama en cada request http_Get en el servidor*/
    /* tenemos habilitado get y root_get_handler*/
};

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
        
    // Start the httpd server
    ESP_LOGI(TAG, "Starting server");

    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();

    /*El modo por defecto es SSL en el puerto 443, hemos elegido 
    *modo inseguro para que pueda trabajar en el puerto 80 para 
    *el navegador no diga que no se tiene certificado*/
    conf.transport_mode = HTTPD_SSL_TRANSPORT_INSECURE;

    esp_err_t ret = httpd_ssl_start(&server, &conf);
    if (ESP_OK != ret) {
        ESP_LOGI(TAG, "Error starting server!");
        return NULL;
    }

    // Set URI handlers, incia server

            ESP_LOGI(TAG, "Registering URI handlers");
            httpd_register_uri_handler(server, &root);
             return server;
 
   
}

static void stop_webserver(httpd_handle_t server)
{
    // Stop the httpd server
    httpd_ssl_stop(server);
}

static void disconnect_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    httpd_handle_t *server = (httpd_handle_t*) arg;
    if (*server) 
    {
            stop_webserver(*server);
            *server = NULL;
        
    }
}

static void connect_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server == NULL) 
    {
        *server = start_webserver();
    }
}

/*---------------------------------------------------------------
        WIFI CONFIGURATION
---------------------------------------------------------------*/
static void initialise_wifi(void)
{ 
   static httpd_handle_t server = NULL;
    
    
    ESP_ERROR_CHECK(esp_netif_init());
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );

    ESP_ERROR_CHECK( esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL) );
    ESP_ERROR_CHECK( esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL) );
    ESP_ERROR_CHECK( esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL) );

    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_start() );
      
    /* CONFIG_EXAMPLE_CONNECT_WIFI
     *Register event handlers to start server when Wi-Fi is connected,
     * and stop server when disconnection happens.
     */
    ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));
    
       
  
}
/*---------------------------------------------------------------
        LED CONFIGURATION
---------------------------------------------------------------*/
esp_err_t init_led(void)
{
    gpio_config_t pGPIOConfig;
    pGPIOConfig.pin_bit_mask = (1ULL << led1);
    pGPIOConfig.mode = GPIO_MODE_DEF_OUTPUT;
    pGPIOConfig.pull_up_en = GPIO_PULLUP_DISABLE;
    pGPIOConfig.pull_down_en = GPIO_PULLDOWN_DISABLE;
    pGPIOConfig.intr_type = GPIO_INTR_DISABLE;

    gpio_config(&pGPIOConfig);

    ESP_LOGI(TAG, "init led completed");
    return ESP_OK;
}

/*esp_err_t toggle_led(int led)
{
    int8_t state = 0;
    switch (led)
    {
    case led1:
        led_1_state = !led_1_state;
        state = led_1_state;
        break;

    default:
        gpio_set_level(led1, 0);
  
        led_1_state = 0;

        break;
    }
    gpio_set_level(led, state);
    return ESP_OK;
}*/

/*---------------------------------------------------------------
        ADC INPUT CONFIGURATION
---------------------------------------------------------------*/
esp_err_t set_adc(void)
{
    //-------------ADC1 Init---------------//

    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));
    //-------------ADC1 Config---------------//
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_11,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, EXAMPLE_ADC1_CHAN0, &config));

   return ESP_OK;


    /*adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_11);
    adc1_config_width(ADC_WIDTH_BIT_12);

    return ESP_OK;*/
}

static esp_err_t refreshAdc()
{
    ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, EXAMPLE_ADC1_CHAN0, &adc_raw[0][0]));
    ESP_LOGI(TAG1, "ADC%d Channel[%d] Raw Data: %d", ADC_UNIT_1 + 1, EXAMPLE_ADC1_CHAN0, adc_raw[0][0]);
    ESP_LOGI(TAG1, "ADC%d Channel[%d] Cali Voltage: %1.4f V", ADC_UNIT_1 + 1, EXAMPLE_ADC1_CHAN0, adc_raw[0][0]/1241.0);
    adc_1_value=  adc_raw[0][0]/1241.0;

    return ESP_OK;
}



void app_main(void)
{
    /*Llamada a inicializar cada componente del web server */
    ESP_ERROR_CHECK(init_led());
    ESP_ERROR_CHECK(set_adc());

    /*Proceso que se ejecuta en respuesta a una solicitud realizada a una app web*/
   ESP_ERROR_CHECK(nvs_flash_init());
    initialise_wifi();

}
