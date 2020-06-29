#include "ets_sys.h"
#include "gpio.h"
#include "hw_timer.c"
#include "mem.h"
#include "os_type.h"
#include "osapi.h"
#include "sntp.h"
#include "uart.h"
#include "user_interface.h"

#include "ip_addr.h"
#include "espconn.h"

#include "wifi_credentials.h"

// Adjustments based on external measurements
#define T_CALIB  -3
#define RH_CALIB +1

#define STARTING_DELAY_US     (2*1000000)
#define CONNECTION_DELAY_US   (5*1000000)
#define SLEEP_DELAY_US        (5*1000000)
#define SLEEP_TIMEOUT_US     (10*1000000)

// The maximum value for PAUSE_TIME_US is the same as the maximum for
// `wifi_fpm_do_sleep(uint32)`.
#define PAUSE_TIME_US        (10*1000000)

static volatile enum
{
    RESET = -1,
    STARTING = 0,
    STARTED = 1,
    WAITING = 2,
    RECEIVING_RESPONSE = 3,
    READY = 4,
    STARTING_DATA = 5,
    RECEIVING_ZERO = 6,
    RECEIVING_ONE = 7,
    RECEIVED_DATA = 8,
    ERROR = 9,
    DONE = 10,
    PRE_SLEEP = 11,
    SLEEP = 12,
    WAKEUP = 13,
    POST_WAKEUP = 14
} state = WAKEUP, previous_state = WAKEUP;

static const int pin = 2;

static volatile uint32_t timer_interval_us = 10;
static volatile uint32_t reset_timer_elapsed = 0;
static volatile uint32_t hw_timer_elapsed = 0;

static volatile int data = 0;
static volatile int checksum = 0;
static volatile int data_counter = 0;

static struct espconn espconn;
static ip_addr_t ip_addr;

static volatile int espconn_disconnecting = 0;

void ICACHE_FLASH_ATTR conn_receive_cb(void *arg, char *pdata, unsigned short len)
{
    struct espconn *espconn = (struct espconn *)arg;
#if 0
    os_printf("Size: %d, Received: %s\r\n", len, pdata);
#else
    os_printf("Received %d bytes\r\n", len);
#endif
}

void ICACHE_FLASH_ATTR conn_sent_cb(void *arg)
{
    os_printf("Sent\r\n");
}

void ICACHE_FLASH_ATTR conn_reconnect_cb(void *arg, sint8 err)
{
    os_printf("Reconnection status: %d\r\n", err);
}

void ICACHE_FLASH_ATTR conn_disconnect_cb(void *arg)
{
    espconn_disconnecting = 0;
    struct espconn *espconn = (struct espconn *)arg;
    os_free(espconn->proto.tcp);
    os_printf("Disconnected\r\n");
}

void ICACHE_FLASH_ATTR conn_connect_cb(void *arg)
{
    struct espconn *espconn = (struct espconn *)arg;

    espconn_regist_disconcb(espconn, conn_disconnect_cb);
    espconn_regist_recvcb(espconn, conn_receive_cb);
    espconn_regist_sentcb(espconn, conn_sent_cb);

    char *data = "GET / HTTP/1.1\r\nHost: vpn.priscimon.net\r\nAccept:*/*\r\n\r\n";
    sint8 result = espconn_secure_send(espconn, data, os_strlen(data));

    os_printf("Send status: %d\r\n", result);
}

void ICACHE_FLASH_ATTR conn_hostfound_cb(const char *name, ip_addr_t *ip_addr, void *arg) {
    struct espconn *espconn = (struct espconn *)arg;
    if (ip_addr != NULL)
    {
        os_printf("Hostname resolved '%s':  %d.%d.%d.%d\r\n",
                name,
                *((uint8 *)&ip_addr->addr),     *((uint8 *)&ip_addr->addr + 1),
                *((uint8 *)&ip_addr->addr + 2), *((uint8 *)&ip_addr->addr + 3));

        espconn->type = ESPCONN_TCP;
        espconn->state = ESPCONN_NONE;
        espconn->proto.tcp = (esp_tcp *)os_zalloc(sizeof(esp_tcp));
        espconn->proto.tcp->local_port = espconn_port();
        espconn->proto.tcp->remote_port = 443;
        os_memcpy(espconn->proto.tcp->remote_ip, &(ip_addr->addr), 4);

        espconn_regist_connectcb(espconn, conn_connect_cb);
        espconn_regist_reconcb(espconn, conn_reconnect_cb);

        espconn_secure_set_size(ESPCONN_CLIENT, 8192);
        espconn_secure_ca_enable(ESPCONN_CLIENT, 0xAA);
        sint8 result = espconn_secure_connect(espconn);
        os_printf("Connection status: %d\r\n", result);
    }
    else
    {
        os_printf("Cannot resolve '%s'\r\n", name);
    }
}

void reset()
{
    os_printf("Resetting...\r\n");

    data = 0;
    checksum = 0;
    data_counter = 0;

    state = STARTING;
    hw_timer_elapsed = 0;

    timer_interval_us = 10;

    uint32 current_timestamp = sntp_get_current_timestamp();

    os_printf("\r\n");
    os_printf("Unix timestamp: %d, Time (UTC): %s\r\n",
            current_timestamp, sntp_get_real_time(current_timestamp));
}

void starting(void)
{
    GPIO_OUTPUT_SET(pin, ~(GPIO_INPUT_GET(pin)));

    if (hw_timer_elapsed > STARTING_DELAY_US)
    {
        GPIO_OUTPUT_SET(pin, 0);

        state = STARTED;
        hw_timer_elapsed = 0;
    }
}

void started(void)
{
    if (hw_timer_elapsed > 18000)
    {
        GPIO_OUTPUT_SET(pin, 1);
        GPIO_DIS_OUTPUT(pin);

        state = WAITING;
        hw_timer_elapsed = 0;
    }
}

void waiting(void)
{
    if (hw_timer_elapsed > 40)
    {
        state = ERROR;
        return;
    }
    if (!GPIO_INPUT_GET(pin))
    {
        state = RECEIVING_RESPONSE;
        hw_timer_elapsed = 0;
    }
}

void receiving_response(void)
{
    if (hw_timer_elapsed > 80)
    {
        state = ERROR;
        return;
    }

    if (GPIO_INPUT_GET(pin))
    {
        state = READY;
        hw_timer_elapsed = 0;
    }
}

void ready(void)
{
    if (hw_timer_elapsed > 80)
    {
        state = ERROR;
        return;
    }

    if (!GPIO_INPUT_GET(pin))
    {
        state = STARTING_DATA;
        hw_timer_elapsed = 0;
    }
}

void starting_data(void)
{
    if (hw_timer_elapsed > 50)
    {
        state = ERROR;
        return;
    }

    if (GPIO_INPUT_GET(pin))
    {
        state = RECEIVING_ZERO;
        hw_timer_elapsed = 0;
    }
}

void receiving_zero(void)
{
    if (hw_timer_elapsed > 28 && GPIO_INPUT_GET(pin))
    {
        state = RECEIVING_ONE;
        return;
    }

    if (!GPIO_INPUT_GET(pin))
    {
        if (data_counter < 32)
        {
            data = data << 1;
        }
        else
        {
            checksum = checksum << 1;
        }

        if (++data_counter < 40)
        {
            state = STARTING_DATA;
            hw_timer_elapsed = 0;
        }
        else
        {
            state = RECEIVED_DATA;
        }
    }
}

void receiving_one(void)
{
    if (hw_timer_elapsed > 70 && GPIO_INPUT_GET(pin))
    {
        state = ERROR;
        return;
    }

    if (!GPIO_INPUT_GET(pin))
    {
        if (data_counter < 32)
        {
            data = (data << 1) | 1;
        }
        else
        {
            checksum = (checksum << 1) | 1;
        }

        if (++data_counter < 40)
        {
            state = STARTING_DATA;
            hw_timer_elapsed = 0;
        }
        else
        {
            state = RECEIVED_DATA;
        }
    }
}

void received_data(void)
{
    espconn_gethostbyname(&espconn, "vpn.priscimon.net", &ip_addr, conn_hostfound_cb);
    timer_interval_us = CONNECTION_DELAY_US;

    int rh_integral = data >> 24;
    int rh_decimal = (data & ~(0xFF << 24)) >> 16;
    int t_integral = (data & ~(0xFFFF << 16)) >> 8;
    int t_decimal = (data & ~(0xFFFFFF << 8));

    int x = rh_integral + rh_decimal + t_integral + t_decimal;

    os_printf("\r\n");
    os_printf("32-bit data: %d\r\n", data);
    os_printf("Temp: %d.%d - RH (%c): %d.%d\r\n",
            t_integral + T_CALIB, t_decimal, '%', rh_integral + RH_CALIB, rh_decimal);
    os_printf("Checksum (calculated/supplied): %d/%d\r\n", x, checksum);
    os_printf("\r\n");

    state = DONE;
}

void error(void)
{
    os_printf("Error: %d\r\n", previous_state);
    state = DONE;
}

void done(void)
{
    os_printf("Done\r\n");

    timer_interval_us = 1000;
    state = PRE_SLEEP;
}

void pre_sleep(void)
{
    os_printf("Pre-sleeping...\r\n");
    sntp_stop();

    espconn_disconnecting = 1;
    espconn_secure_disconnect(&espconn);

    wifi_station_disconnect();
    wifi_set_opmode(NULL_MODE);

    os_printf("Going in light sleep for %d ms...\r\n", PAUSE_TIME_US/1000);

    state = SLEEP;
    hw_timer_elapsed = 0;

    timer_interval_us = SLEEP_DELAY_US;
    hw_timer_arm(timer_interval_us);
}

void wifi_wakeup_cb(void)
{
    state = WAKEUP;
    hw_timer_elapsed = 0;

    hw_timer_arm(timer_interval_us);
}

void sleep(void)
{
    if (espconn_disconnecting && hw_timer_elapsed < SLEEP_TIMEOUT_US)
    {
        hw_timer_arm(timer_interval_us);
        return;
    }


    wifi_fpm_set_sleep_type(LIGHT_SLEEP_T);
    wifi_fpm_open();
    wifi_fpm_set_wakeup_cb(wifi_wakeup_cb);
    wifi_fpm_do_sleep(PAUSE_TIME_US);
}

void wakeup(void)
{
    os_printf("Waking up...\r\n");

    wifi_fpm_do_wakeup();
    wifi_fpm_close();
    wifi_fpm_set_sleep_type(NONE_SLEEP_T);
    wifi_set_opmode(STATION_MODE);
    wifi_station_connect();

    sntp_setservername(0, "ntp01.algon.dk");
    sntp_setservername(1, "ntp.uit.one");
    sntp_setservername(2, "time.windows.com");
    sntp_set_timezone(0);
    sntp_init();

    state = POST_WAKEUP;
    hw_timer_elapsed = 0;

    timer_interval_us = CONNECTION_DELAY_US;
}

void post_wakeup()
{
    uint32_t current_timestamp = sntp_get_current_timestamp();

    if (current_timestamp == 0)
        return;

    state = RESET;
    hw_timer_elapsed = 0;
}

void hw_timerfunc(void)
{
    hw_timer_elapsed += timer_interval_us;

    if (state != ERROR)
        previous_state = state;

    switch (state)
    {
        case RESET:
            reset();
            break;
        case STARTING:
            starting();
            break;
        case STARTED:
            started();
            break;
        case WAITING:
            waiting();
            break;
        case RECEIVING_RESPONSE:
            receiving_response();
            break;
        case READY:
            ready();
            break;
        case STARTING_DATA:
            starting_data();
            break;
        case RECEIVING_ZERO:
            receiving_zero();
            break;
        case RECEIVING_ONE:
            receiving_one();
            break;
        case RECEIVED_DATA:
            received_data();
            break;
        case ERROR:
            error();
            break;
        case DONE:
            done();
            break;
        case PRE_SLEEP:
            pre_sleep();
            break;
        case SLEEP:
            sleep();
            break;
        case WAKEUP:
            wakeup();
            break;
        case POST_WAKEUP:
            post_wakeup();
            break;
    }

    if (state != SLEEP)
        hw_timer_arm(timer_interval_us);
}

void ICACHE_FLASH_ATTR start_wifi_station(void)
{
    wifi_set_opmode(STATION_MODE);
    wifi_fpm_set_sleep_type(MODEM_SLEEP_T);

    // SSID and PASSWORD must be defined in wifi_credentials.h
    char ssid[32] = SSID;
    char password[64] = PASSWORD;

    struct station_config stationConf;
    stationConf.bssid_set = 0; //need not check MAC address of AP

    os_memcpy(&stationConf.ssid, ssid, 32);
    os_memcpy(&stationConf.password, password, 64);
    wifi_station_set_config(&stationConf);
}

void ICACHE_FLASH_ATTR user_init(void)
{
    uart_init(BIT_RATE_115200, BIT_RATE_115200);
    start_wifi_station();

    gpio_init();
    PIN_PULLUP_DIS(PERIPHS_IO_MUX_GPIO2_U);

    hw_timer_init(FRC1_SOURCE, 0);
    hw_timer_set_func(hw_timerfunc);
    hw_timer_arm(timer_interval_us);
}
