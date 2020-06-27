#include "ets_sys.h"
#include "gpio.h"
#include "hw_timer.c"
#include "os_type.h"
#include "osapi.h"
#include "uart.h"
#include "user_interface.h"
#include "wifi_credentials.h"

#define T_CALIB     -3
#define RH_CALIB    +1

#define PAUSE_TIME_US   60000000

static volatile enum
{
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
    RESET = 10,
    DONE = 11
} state = RESET, previous_state = RESET;

static const int pin = 2;

static volatile int hw_timer_interval = 1000000;
static volatile int reset_timer_elapsed = 0;
static volatile int hw_timer_elapsed = 0;

static volatile int data = 0;
static volatile int checksum = 0;
static volatile int data_counter = 0;

static os_timer_t os_timer;

void starting(void)
{
    GPIO_OUTPUT_SET(pin, ~(GPIO_INPUT_GET(pin)));
    if (hw_timer_elapsed > 1000)
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
    int rh_integral = data >> 24;
    int rh_decimal = (data & ~(0xFF << 24)) >> 16;
    int t_integral = (data & ~(0xFFFF << 16)) >> 8;
    int t_decimal = (data & ~(0xFFFFFF << 8));

    int x = rh_integral + rh_decimal + t_integral + t_decimal;

    os_printf("\r\n");
    os_printf("32-bit data: %d\r\n", data);
    os_printf("Temp: %d.%d - RH (%c): %d.%d\r\n", t_integral + T_CALIB, t_decimal, '%', rh_integral + RH_CALIB, rh_decimal);
    os_printf("Checksum (calculated/supplied): %d/%d\r\n", x, checksum);
    os_printf("\r\n");

    state = DONE;
}

void error(void)
{
    os_printf("ERR: %d\r\n", previous_state);
    state = RESET;
}

void reset(void)
{
    if (reset_timer_elapsed > PAUSE_TIME_US)
    {
        state = STARTING;
        reset_timer_elapsed = 0;

        data = 0;
        checksum = 0;
        data_counter = 0;

        hw_timer_interval = 10;
        hw_timer_elapsed = 0;

        wifi_fpm_do_wakeup();
        wifi_fpm_close();
        wifi_set_opmode(STATION_MODE);
        wifi_station_connect();
    }
    else if (hw_timer_interval != 1000000)
    {
        hw_timer_interval = 1000000;
        os_printf("HW timer interval set to %d\r\n", hw_timer_interval);

        wifi_station_disconnect();
        wifi_set_opmode(NULL_MODE);
        wifi_fpm_open();
        wifi_fpm_do_sleep(PAUSE_TIME_US);
    }
    else
    {
        reset_timer_elapsed += hw_timer_interval;
    }
}

void done(void)
{
    state = RESET;
}


void hw_timerfunc(void)
{
    hw_timer_elapsed += hw_timer_interval;

    if (state != ERROR)
        previous_state = state;

    switch (state)
    {
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
        case RESET:
            reset();
            break;
        case DONE:
            done();
            break;
    }

    hw_timer_arm(hw_timer_interval);
}

void ICACHE_FLASH_ATTR
user_set_station_config(void)
{
    wifi_set_sleep_type(MODEM_SLEEP_T);
    wifi_set_opmode(STATION_MODE);

    // SSID and PASSWORD must be defined in wifi_credentials.h
    char ssid[32] = SSID;
    char password[64] = PASSWORD;

    struct station_config stationConf;
    stationConf.bssid_set = 0; //need not check MAC address of AP

    os_memcpy(&stationConf.ssid, ssid, 32);
    os_memcpy(&stationConf.password, password, 64);
    wifi_station_set_config(&stationConf);
}

void ICACHE_FLASH_ATTR user_init()
{
    uart_init(BIT_RATE_115200, BIT_RATE_115200);

    user_set_station_config();

    gpio_init();
    PIN_PULLUP_DIS(PERIPHS_IO_MUX_GPIO2_U);

    hw_timer_init(FRC1_SOURCE, 0);
    hw_timer_set_func(hw_timerfunc);
    hw_timer_arm(hw_timer_interval);
}
