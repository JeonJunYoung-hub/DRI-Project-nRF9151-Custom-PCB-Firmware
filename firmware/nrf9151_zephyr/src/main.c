/*********
  LTE Environmental Sensor Example
  LTE/MQTT communication example for environmental telemetry
 
  Sanitized public example
 
  Last update: 2025.12.16
  Code description:
    - Sensor reports every 20min based on RTC(Real Time Clock)
    - Voltage detection and sleep
    - PSM configuration for low power consumption
    - Watchdog configuration for automatic reset    
 
  PCB specifications;
  - Custom board which is based on nRF9151 DK chip set
  - LTE-M/NB-IoT SIM card
  - 3.3V connector
  - Carrier PCB with I2C multiplexor,
    - UART #0: Communication with debuger which is SEGGER J-Link mini
    - GPIO #1: GPIO 1 is disabled because it is used by default in TF-M, reserved for the Secure world (Using silent log API it is solved)
    - I2C #1: Sensirion SEN66 PM, VOC, NOx, Temp, RH sensor
    - I2C #2: Bosch BMP581 Temp, Pressure sensor
    - SPI #3: GigaDevice gd25wb256e flash memory 256Mbit(32Mbyte)
    
*********/
#include <zephyr/debug/thread_analyzer.h>


#include <stdio.h>
#include <ncs_version.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/reboot.h>

/* MultiThread */
#define STACK_SIZE 4096
#define THREAD_PRIORITY 3

/* Configuration */
#include <config.h>

/* SPI */
#include <zephyr/IOT_PIPELINEvers/spi.h>
#include <zephyr/IOT_PIPELINEvers/flash.h>
#include <zephyr/storage/flash_map.h>

const struct device *flash_dev = DEVICE_DT_GET(DT_NODELABEL(gd25wb256));

/* ADC */
#include <zephyr/IOT_PIPELINEvers/adc.h>

/* Device ID */
#include <zephyr/IOT_PIPELINEvers/hwinfo.h>

/* SIM CARD Communication */
#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>


/* Mqtt connection*/
#include <zephyr/net/mqtt.h>
#include "mqtt_connection.h"


/* I2C Headers */
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/IOT_PIPELINEvers/i2c.h>


/* Pressure BMP581 */
#include "bmp5.h"
#include "bmp5_defs.h"

/* Gnss */
#include <nrf_modem_gnss.h>

/* Watchdog for reset */
#include <zephyr/IOT_PIPELINEvers/watchdog.h>

/* Date time */
#include <date_time.h>
#include <zephyr/IOT_PIPELINEvers/counter.h>



/* LED */
#include <zephyr/IOT_PIPELINEvers/gpio.h>
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* Device ID */
char device_id_str[10] = {0};
char topic_sub[64] = {0};

uint8_t id[8];
int device_id(void)
{
    
hwinfo_get_device_id(id, sizeof(id));

snprintf(device_id_str,sizeof(device_id_str) , "%02x%02x%02x",
       id[0], id[1], id[2]);


printk("... \r\n");

printk("[Device ID]  %02x%02x%02x\n",
       id[0], id[1], id[2]);
       
printk("... \r\n");
k_sleep(K_SECONDS(1));

}


/* SEN66 */
#define SEN66_NODE DT_NODELABEL(sen66)
#define SEN66_1_NODE DT_NODELABEL(sen66_1)

#define EXUCUTION_TIME 25
#define STARTING_TIME 3000
#define DEVICE_ID_WATTING_TIME 100
#define RECONNECTING_TIME 15000

#define GAP 200

/* SEN66 Function */

int read_measurement(const struct i2c_dt_spec *dev, uint8_t *buf, size_t len)
{
    int ret;
    uint8_t cmd_read[] = {0x03, 0x00};

    ret = i2c_write(dev->bus, cmd_read, sizeof(cmd_read), dev->addr);
    if (ret != 0) {
        printk("[SEN] Failed to send read command: %d\r\n", ret);
        return ret;
    }

    k_msleep(30);  

    ret = i2c_read(dev->bus, buf, len, dev->addr);
    if (ret != 0) {
        printk("[SEN] Failed to read measurement: %d\r\n", ret);
        return ret;
    }
      
    return 0;  
}

int read_raw_measurement(const struct i2c_dt_spec *dev, uint16_t *buf, size_t len)
{
    
    int ret;
    uint8_t cmd_read[] = {0x04, 0x05};

    ret = i2c_write(dev->bus, cmd_read, sizeof(cmd_read), dev->addr);
    if (ret != 0) {
        printk("[SEN] Failed to send read command: %d\r\n", ret);
        return ret;
    }

    k_msleep(30); 

    ret = i2c_read(dev->bus, buf, len, dev->addr);
    if (ret != 0) {
        printk("[SEN] Failed to read measurement: %d\r\n", ret);
        return ret;
    }

    return 0;  
}


uint16_t read_u16(uint8_t msb, uint8_t lsb) {
    return ((uint16_t)msb << 8) | lsb;
}

int16_t read_s16(uint8_t msb, uint8_t lsb) {
    return (int16_t)(((uint16_t)msb << 8) | lsb);  // - value possible
}


/* BMP581 */
#define BMP581_NODE DT_NODELABEL(i2c2)
#define BMP581_ADDR 0x46

const struct device *i2c_dev;


/* BMP581 Function */
struct bmp5_dev bmp_dev;
struct bmp5_osr_odr_press_config osr_cfg;
struct bmp5_sensor_data sensor_data;

BMP5_INTF_RET_TYPE user_i2c_read(uint8_t reg_addr, uint8_t *data, uint32_t len, void *intf_ptr) {
    const struct device *i2c = (const struct device *)intf_ptr;
    return i2c_write_read(i2c, BMP581_ADDR, &reg_addr, 1, data, len) == 0 ? BMP5_OK : BMP5_E_COM_FAIL;

}

 
BMP5_INTF_RET_TYPE user_i2c_write(uint8_t reg_addr, const uint8_t *data, uint32_t len, void *intf_ptr) {
    const struct device *i2c = (const struct device *)intf_ptr;
    uint8_t buf[1 + len];
    buf[0] = reg_addr;
    memcpy(&buf[1], data, len);
    return i2c_write(i2c, buf, len + 1, BMP581_ADDR) == 0 ? BMP5_OK : BMP5_E_COM_FAIL;
}

void user_delay_us(uint32_t period, void *intf_ptr) {
    k_usleep(period);
}


/* THE MQTT CONNECTION FOUNCTION */
static struct mqtt_client client;

/* File descriptor */
static struct pollfd fds;

static K_SEM_DEFINE(lte_connected, 0, 1);
K_SEM_DEFINE(gnss_done, 0, 1);

LOG_MODULE_REGISTER(IOT_PIPELINE, LOG_LEVEL_INF);


static void lte_handler(const struct lte_lc_evt *const evt)
{
     switch (evt->type) {
     case LTE_LC_EVT_NW_REG_STATUS:
        if ((evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_HOME) &&
            (evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_ROAMING)) {
            break;
        }
		LOG_INF("[LTE] Network registration status: %s\r\n",
				evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ?
				"Connected - home network" : "Connected - roaming");
		k_sem_give(&lte_connected);
        break;

        case LTE_LC_EVT_RRC_UPDATE:
            LOG_INF("[LTE] RRC: %s\r\n", evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ?               
                       "CONNECTED" : "IDLE          ");
            break;

        case LTE_LC_EVT_PSM_UPDATE:
            
            printk("[PSM] PSM TAU=%d s, ActiveTime=%d s \r\n",evt->psm_cfg.tau, evt->psm_cfg.active_time);
            printk("...\r\n");
            k_msleep(GAP);
            break;
        
     default:
             break;
     }
}

static int modem_configure(void)
{
	int err;
    
    printk("...\r\n");
	printk("[MODEM] Initializing modem library\r\n");

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("Failed to initialize the modem library, error: %d \r\n", err);
		return err;
	}else {
        printk("[MODEM] Success to initialize the modem library\r\n");
    }

    printk("...\r\n");
    printk("...\r\n");
    k_msleep(GAP);

}


static int lte_connect_function(void)
{
       
    gpio_pin_set_dt(&led, 1);

    int err;
    int count = 0;

    lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_LTE);
    k_msleep(5000);

    connect_lte:
    err = lte_lc_connect_async(lte_handler);
    if (err) {
        LOG_ERR("Error in lte_lc_connect_async, error: %d", err);
        count++;

        if (count >= 3) {
            printk("[LTE] LTE connect failed 3 times → skip LTE\n");
            return -1;   
        }

        goto connect_lte;
    }

    printk("[LTE] LTE is connecting...\r\n");

    k_sem_take(&lte_connected, K_FOREVER);
        
    printk("[LTE] Connected to LTE network\r\n");



    printk("...\r\n");
    printk("...\r\n");
    k_msleep(GAP);

    return 0;
}




static void maintain_mqtt(void)
{
    
    int rc = poll(&fds, 1, 0);
    if (rc < 0) {
        LOG_ERR("poll 에러: %d", errno);
        return;
    }
   
    if (rc > 0) {
        mqtt_input(&client);
    }
    
    rc = mqtt_live(&client);
    if (rc && rc != -EAGAIN) {
        LOG_ERR("mqtt_live 에러: %d", rc);
    }
}


/* GNSS Macro, struct */
static struct nrf_modem_gnss_pvt_data_frame pvt_data;
static int64_t gnss_start_time;

volatile bool STOP = false;
volatile bool gnss_fix_success = false;

static K_SEM_DEFINE(gnss_fixed, 0, 1);
static K_SEM_DEFINE(GNSS_retry,0,1);

uint8_t gnss_flag;



/* GNSS Function */
static void print_fix_data(struct nrf_modem_gnss_pvt_data_frame *pvt_data)
{
	LOG_INF("Latitude:       %.06f", pvt_data->latitude);
	LOG_INF("Longitude:      %.06f", pvt_data->longitude);
	LOG_INF("Altitude:       %.01f m", (double)pvt_data->altitude);
    
}

struct tm now_tm;

static void gnss_event_handler(int event)
{
	int err;

	switch (event) {
	
	case NRF_MODEM_GNSS_EVT_PVT:
		LOG_INF("Searching...");

       int num_satellites = 0;

        for (int i = 0; i < 12 ; i++) {
        if (pvt_data.sv[i].signal != 0) {
        LOG_INF("sv: %d, cn0: %d", pvt_data.sv[i].sv, pvt_data.sv[i].cn0);
        num_satellites++;
        }  
        }
        LOG_INF("Number of satellites: %d", num_satellites);

		err = nrf_modem_gnss_read(&pvt_data, sizeof(pvt_data), NRF_MODEM_GNSS_DATA_PVT);
		if (err) {
			LOG_ERR("nrf_modem_gnss_read failed, err %d", err);
			return;
		}

		if (pvt_data.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {

			print_fix_data(&pvt_data);
			k_sem_give(&gnss_fixed);
            
		}

		break;

    case NRF_MODEM_GNSS_EVT_SLEEP_AFTER_TIMEOUT:
        

        k_sem_give(&GNSS_retry);
        printk("[GNSS] GNSS Timeout\r\n");
        break;
    }
} 

#define LOG_OFFSET_GNSS      0x01F00000 // 24MB
volatile uint64_t now_ms = 0;

void save_gnss_to_flash(float latitude, float longitude, float altitude, uint64_t now_ms)
{
    /* GNSS Data to publish*/
    char GNSS_Data[512] = {0};
    int GNSS_Data_String = snprintf(GNSS_Data, sizeof(GNSS_Data),
        "{"
            "\"ID\":\"%s\","
            "\"latitude\":%.6f,"
            "\"longitude\":%.6f,"
            "\"altitude\":%.2f"
            "}",
            device_id_str, latitude, longitude, altitude);

    
    printk("[FLASH] JSON to save = %s\n", GNSS_Data);
    
       
        flash_erase(flash_dev, LOG_OFFSET_GNSS, 4096);
        
        int err = flash_write(flash_dev, LOG_OFFSET_GNSS , GNSS_Data , GNSS_Data_String);
        if (err == 0){
            printk("[FLASH] Flash write success addr: 0x%08x data: %s \r\n", LOG_OFFSET_GNSS, GNSS_Data);
        }else {
            LOG_ERR("[FLASH] Flash write failed %d \r\n", err);
        }
   
}

   #include <inttypes.h>
    
void time_convert(void)
{
    
    date_time_now(&now_ms);
    
   printk("[Time] UTC epoch(ms): %llu\n", now_ms);
}

static int gnss_fix_once(void)
{

    printk("[GNSS] Start GNSS one-time fix\r\n");
    printk("[GNSS] GNSS TIMEOUT = %d\n", CONFIG_GNSS_PERIODIC_TIMEOUT);

    if (lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_GNSS) != 0) {
		LOG_ERR("Failed to activate GNSS functional mode");
		return 0;
	} else {
		printk("[GNSS] SUCCESS TO CONFIGURE THE MODEM\r\n");
	}
    
    if (nrf_modem_gnss_event_handler_set(gnss_event_handler) != 0) {
		LOG_ERR("Failed to set GNSS event handler");
		return 0;
	} else {
		printk("[GNSS] SUCCESS TO set gnss event handler THE MODEM\r\n");
	}

    if(nrf_modem_gnss_fix_interval_set((0)!= 0)){
        LOG_ERR("Failed to set GNSS fix interval");
        return 0;
    }

	if (nrf_modem_gnss_fix_retry_set(CONFIG_GNSS_PERIODIC_TIMEOUT) != 0) { // set time out 10mins
		LOG_ERR("Failed to set GNSS fix retry");
        return 0;
	}
    
    printk("[GNSS] Starting GNSS\r\n");

	if (nrf_modem_gnss_start() != 0) {
		LOG_ERR("Failed to start GNSS");
	}

    while(1)
    {
    struct k_poll_event events[2] = {
            K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_SEM_AVAILABLE,
                                     K_POLL_MODE_NOTIFY_ONLY, &gnss_fixed),
            K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_SEM_AVAILABLE,
                                     K_POLL_MODE_NOTIFY_ONLY, &GNSS_retry),
        };

    k_poll(events, ARRAY_SIZE(events), K_FOREVER);

    if (events[0].state == K_POLL_STATE_SEM_AVAILABLE) {
            
            k_sem_take(&gnss_fixed, K_NO_WAIT);
           int err = nrf_modem_gnss_stop();
            if( err == 0){
                printk("[GNSS] GNSS Stop success\r\n");
                lte_lc_func_mode_set(LTE_LC_FUNC_MODE_DEACTIVATE_GNSS);
                k_sleep(K_MSEC(500));
                k_sem_give(&gnss_done);
            }
            save_gnss_to_flash(pvt_data.latitude, pvt_data.longitude, pvt_data.altitude, now_ms);

            gpio_pin_set_dt(&led, 0);

            printk("[GNSS] Fix success, stopping GNSS\n");
            printk("... \r\n");
            printk("... \r\n");
            k_sleep(K_SECONDS(8));
            break;
        }

    if (events[1].state == K_POLL_STATE_SEM_AVAILABLE) {
            
            k_sem_take(&GNSS_retry, K_NO_WAIT);
            int err = nrf_modem_gnss_stop();
            if( err == 0){
                printk("[GNSS] GNSS Stop success\r\n");
                lte_lc_func_mode_set(LTE_LC_FUNC_MODE_DEACTIVATE_GNSS);
                k_sleep(K_MSEC(500));
                k_sem_give(&gnss_done);
            }
            

            gpio_pin_set_dt(&led, 0);

            printk("[GNSS] Timeout just send last GNSS Data\n");
            printk("... \r\n");
            printk("... \r\n");
            k_sleep(K_SECONDS(8));

            
            break;
        }

    }
    nrf_modem_gnss_event_handler_set(NULL);
    printk("...\r\n");
    k_msleep(GAP);
    

    return 0;

}



 /* Watch dog */
static const struct device *wdt;
static int wdt_channel = -1;

#define WDT_TIMEOUT_SEC  960000// 15minutes

static void wdt_init_and_start(void)
{
    wdt = DEVICE_DT_GET(DT_NODELABEL(wdt));
    if (!device_is_ready(wdt)) {
        LOG_ERR("Watchdog device is not ready~~");
        return;
    } else {
        printk("[Watch Dog] Watchdog is ready\r\n");
    }

    struct wdt_timeout_cfg cfg = {
        .window = {
            .min = 0,
            .max = WDT_TIMEOUT_SEC, 
        },
        .callback = NULL,
        .flags = WDT_FLAG_RESET_SOC,  
    };
    
    wdt_channel = wdt_install_timeout(wdt, &cfg); // 0 is fail and 1 is success on the setting as a intizer.
    if (wdt_channel < 0) {
        LOG_ERR("[Watch Dog] install_timeout() failed: %d", wdt_channel);
        return;
    } else {
        printk("[Watch Dog] install_timeout() success: %d\r\n", wdt_channel);
    }

	int err = wdt_setup(wdt, 0);
    if (err) {
        LOG_ERR("wdt_setup() failed: %d", err);
        return;
    }
    
        printk("[Watch Dog] started: timeout=%ds, channel=%d\r\n", WDT_TIMEOUT_SEC, wdt_channel);

    printk("... \r\n");
    printk("... \r\n");
    k_msleep(GAP);
}

static inline void wdt_try_feed(void){
    if (wdt_channel >= 0) {
        wdt_feed(wdt, wdt_channel);
        printk("[Watch Dog] Feeding Success \r\n");
        printk("... \r\n");
        k_msleep(GAP);
    }
}
    
    /* PSM Setting */
    void PSM_eDRX(void)
    {
        
        #define PSM_TAU "00100001"      // 10hours        // 000 = 10 mins, 001 = 1 hour, 010 = 10 hours, 011 = 2 seconds, 100 = 30 seconds, 101 = 1 minute, 110 = 320 hours, 111 = deactivated
        #define ACTIVE_TIME "11100000" // Inactive value for active time on psm

        int err = lte_lc_psm_param_set(PSM_TAU, ACTIVE_TIME); 
        if (err) {
            LOG_ERR("Failed to set PSM parameters: %d", err);
            return;
        }
        
        err = lte_lc_psm_req(true);
        if (err) {
            LOG_ERR("lte_lc_psm_req failed: %d", err);
            return;
        }
        
        printk("[PSM] Requested successfully\r\n");
        
        

        printk("...\r\n");
        printk("...\r\n");
        k_msleep(GAP);

    }

    /* The Function of SEN66 MEASUREMENT */

    void SEN66_MEASURE_u16(const struct device *dev, uint8_t *buf, size_t buf_len, float *out_array, int start_index, const char *label, float *avg_out)
    {
        
        float sum = 0.0f;
        printk("...\r\n");
        printk(" -- %s_Array Values -- \r\n",label);
        for ( int i = 0; i <5; i++)
        {
           

            int ret = read_measurement(dev, buf, buf_len);
            if (ret != 0) {
                printk("[SEN] Failed to read measurement: %d\r\n", ret);
                return;
            }
            

            uint16_t raw = read_u16(buf[start_index], buf[start_index + 1]);
            float value = raw / 10.0f; // Assuming the value needs to be divided by 10.0f

            out_array[i] = value;
            sum += value;

            printk("[SEN] %s_array[%d]= %.2f ug/m3 \r\n", label , i , out_array[i]);

            k_msleep(GAP);
        }

        *avg_out = sum / 5.0f;
        printk("[SEN] sum %.2f \r\n",sum);
        
        printk("\r\n");
        sum = 0.0f;

    }

    void SEN66_MEASURE_s16(const struct device *dev, uint8_t *buf, size_t buf_len, float *out_array, int start_index, const char *label, float *avg_out, float divide, const char *unit )
{
    
        float sum = 0.0f;
        printk("...\r\n");
        printk(" -- %s_Array Values -- \r\n",label);
        for ( int i = 0; i <5; i++)
        {
            

            int ret = read_measurement(dev, buf, buf_len);
            if (ret != 0) {
                printk("[SEN] Failed to read measurement: %d\r\n", ret);
                return;
            }

            int16_t raw = read_s16(buf[start_index], buf[start_index + 1]);
            float value = raw / divide; 
            
            out_array[i] = value;
            sum += value;

            printk("[SEN] %s_array[%d]= %.2f %s \r\n", label , i , value, unit);

            k_msleep(GAP);
        }

        *avg_out = sum / 5.0f;
        printk("[SEN] sum %.2f \r\n",sum);
        
        printk("\r\n");

        
        sum = 0.0f;

    }

    void SEN66_MEASURE_CO2(const struct device *dev, uint8_t *buf, size_t buf_len, uint8_t *out_array, int start_index, const char *label, uint16_t *avg_out)
    {
        
        float sum = 0.0f;
        printk("...\r\n");
        printk(" -- %s_Array Values -- \r\n",label);

        for ( int i = 0; i <5; i++)
        {
            

            int ret = read_measurement(dev, buf, buf_len);
            if (ret != 0) {
                printk("[SEN] Failed to read measurement: %d\r\n", ret);
                return;
            }

            uint16_t raw = read_u16(buf[start_index], buf[start_index + 1]);
            out_array[i] = raw;
            sum += raw;

            printk("[SEN] %s_array[%d]= %u ug/m3 \r\n", label , i , raw);

            k_msleep(GAP);
        }

        *avg_out = sum / 5.0f;
        uint16_t avg_co2 = (uint16_t)(*avg_out);
        printk("[SEN] sum %.2f \r\n",sum);
        
        printk("\r\n");
        sum = 0.0f;

    }


    void SEN66_MEASURE_RAW_NOX_VOC(const struct device *dev, uint8_t *buf, size_t buf_len, uint16_t *out_array, int start_index, const char *label, uint16_t *avg_out) //int TIME)
    {
        uint32_t sum = 0;
        
        printk("...\r\n");
        printk(" -- %s_Array Values -- \r\n",label);
        for ( int i = 0; i <5; i++)
        {
           

            int ret = read_raw_measurement(dev, buf, buf_len);
            if (ret != 0) {
                printk("[SEN] Failed to read measurement: %d\r\n", ret);
                return;
            }
            

            uint16_t raw = read_u16(buf[start_index], buf[start_index + 1]);

            out_array[i] = raw;
            sum += raw;

            printk("[SEN] %s_array[%d]= %u ticks \r\n", label , i , out_array[i]);

           k_msleep(GAP);
        }

        *avg_out = sum / 5;
        printk("[SEN] sum %u \r\n",sum);
        
        printk("\r\n");
        sum = 0;

    }


    void MQTT_Reconnect(void)
    {
        printk("[MQTT] MQTT connecting Task Started\r\n");

        mqtt_connected = false;


        int err = client_init(&client);
        if (err) {
            LOG_ERR("Failed to initialize MQTT client: %d", err);

        }

        uint32_t connect_attempt = 0;

        do_connect:
        if (connect_attempt++ > 0) {
            LOG_INF("Reconnecting in %d ms...", RECONNECTING_TIME);
            k_msleep(RECONNECTING_TIME);
        }

        err = mqtt_connect(&client);
        if (err) {
            LOG_ERR("Error in mqtt_connect: %d", err);
            goto do_connect;
        }

        
        err = fds_init(&client,&fds);
        if (err) {
            LOG_ERR("Error in fds_init: %d", err);
        }

       int64_t timeout = k_uptime_get() + 10000; // 10초

        while (!mqtt_connected && k_uptime_get() < timeout) {
            poll(&fds, 1, 200);
            mqtt_input(&client);
        }
            
        if (!mqtt_connected) {
        LOG_ERR("[MQTT] CONNACK timeout");
        return;

        printk("[MQTT] MQTT connected\r\n");
        printk("...\r\n");
        
        k_msleep(3000);

    }
           
        
	printk("[MQTT] Fully connected");
    printk("... \r\n");
}


    /* RTC */

    #define RTC_FREQ 32768
    #define MAX_SECS 300

    const struct device *rtc;
    

    volatile static uint64_t remaining_secs = 0;
    volatile uint64_t target_ms = 0;
    
    uint64_t delay_ms = 0;

    volatile bool Alarm_expired = false;



    /* RTC Function */


static inline uint64_t next_time_set(uint64_t now_ms, const int int_minutes) // 다음 정각을 구함
{
    
    const uint64_t interval_ms = int_minutes * 60 * 1000ULL ; 
    
    uint64_t n = now_ms / interval_ms;

    time_convert();

    return (n + 1ULL) * interval_ms;
}




K_SEM_DEFINE(alarm_sem,0,1);

K_SEM_DEFINE(timing,0,1);

static void every_20mins(const struct device *dev,
                           uint8_t chan_id,
                           uint32_t ticks,
                           void *user_data)
{
    
    printk("[Alarm] Ringed!! remaining_secs=%lld \r\n", remaining_secs);
    
    if (remaining_secs > 0)
    {

        uint32_t next_secs = MIN(remaining_secs, MAX_SECS);
        remaining_secs -= next_secs;

        uint32_t next_ticks = 0;
        next_ticks = next_secs * RTC_FREQ;
        if (next_ticks == 0) next_ticks = 1; // 최소 1
        
    struct counter_alarm_cfg cfg2 = {
            .flags = 0,
            .ticks = next_ticks,
            .callback = every_20mins,
            .user_data = NULL
        };
        
        
        int err2 = counter_set_channel_alarm(dev, 0, &cfg2);
        if (err2) {
                LOG_ERR("Failed2 to set RTC alarm: %d", err2);
            } else {
                printk("[Alarm] Next RTC alarm set after %llu sec \r\n", remaining_secs);
            }

    } else if(remaining_secs == 0){
        
        k_sem_give(&alarm_sem);

        Alarm_expired = true;

        if(Alarm_expired)
        {
            printk("[Alarm] Alarm_expired flag is true \r\n"); 
        }else{
            printk("[Alarm] Alarm_expired flag is false \r\n");
        }
        
        printk("... \r\n");
        printk("... \r\n");

    }



}



static void set_rtc_alarm(uint64_t now_ms, const int int_minutes) {

    if (date_time_now(&now_ms) != 0) {
        printk("[RTC] Failed to get current time\n");
        return;
    }

    
    target_ms =  next_time_set(now_ms, int_minutes); // minutes → ms
    delay_ms = target_ms - now_ms;
    remaining_secs = (delay_ms + 999ULL) / 1000ULL;

    uint32_t first_secs = MIN(remaining_secs, MAX_SECS);
     if (first_secs == 0) first_secs = 1;
     remaining_secs -= first_secs;

    

    uint32_t ticks = first_secs * RTC_FREQ;

    struct counter_alarm_cfg cfg = {
        .flags = 0,
        .ticks = ticks,
        .callback = every_20mins,   // 기존 콜백 그대로 사용
        .user_data = NULL
    };

    int err = counter_set_channel_alarm(rtc, 0, &cfg);
    if (err) {
        LOG_ERR("Failed to set RTC alarm: %d", err);
    } else {
        printk("[RTC] Alarm set for next %llu seconds \n", (unsigned long long)remaining_secs );
    }

    

}



static void reset_rtc_alarm(uint64_t now_ms, volatile int int_minutes) {

   
    date_time_now(&now_ms);

    target_ms = now_ms + (int_minutes * 60 * 1000ULL) ;
    
    uint64_t delay_ms = target_ms - now_ms;

    remaining_secs = (delay_ms + 999ULL) / 1000ULL; //delay_ms / 1000ULL;

    uint64_t first_secs = MIN(remaining_secs, MAX_SECS);
    if (first_secs == 0) first_secs = 1;
    remaining_secs -= first_secs;

    
    uint32_t ticks = 0;
     ticks = first_secs * RTC_FREQ;


    struct counter_alarm_cfg cfg = {
        .flags = 0,
        .ticks = ticks,
        .callback = every_20mins,   // 기존 콜백 그대로 사용
        .user_data = NULL
    };

    
    int err = counter_set_channel_alarm(rtc, 0, &cfg);
    if (err) {
        LOG_ERR("Failed to set RTC alarm: %d", err);
    } else {
        printk("[RTC] Alarm set for next %llu mins \n", (delay_ms + 999ULL)  / 1000ULL );
    }


}



//evt->type == DATE_TIME_OBTAINED_NTP ||

static void dt_evt_handler(const struct date_time_evt *evt)
{
    
    if (evt->type == DATE_TIME_OBTAINED_MODEM ||
        evt->type == DATE_TIME_OBTAINED_NTP ||
        evt->type == DATE_TIME_OBTAINED_EXT ||
        evt->type == DATE_TIME_NOT_OBTAINED) {

    printk("[dt_evt_handler] Date and time event received: %d\r\n", evt->type);


            time_convert();

            /* Measure next time */
            target_ms = next_time_set(now_ms, 20); // Next time set to whatever                        
            delay_ms = target_ms - now_ms;
            remaining_secs = (delay_ms + 999ULL) / 1000ULL;

            uint64_t first_secs = MIN(remaining_secs, MAX_SECS);
            remaining_secs -= first_secs;

            uint32_t ticks = 0;
            ticks = first_secs * RTC_FREQ;

            if (ticks == 0) ticks = 1; // 최소 1
      
            struct counter_alarm_cfg cfg = {
                .flags = 0,
                .ticks = ticks,
                .callback = every_20mins,
                .user_data = NULL
            };

            int err = counter_start(rtc);
            if(err)
            {
                printk("[dt_evt_handler] counter has started %d\r\n", err);
            }

        err = counter_set_channel_alarm(rtc, 0, &cfg); 
        if(err)
            {
                printk("counter fail \r\n");
            }else{
                printk("[RTC] Counter set next %llu Secs\r\n", (delay_ms + 999ULL) / 1000ULL);
            }
        
            
        }
}



static void date_time_evt_handler(const struct date_time_evt *evt)
{
    switch (evt->type) {
    case DATE_TIME_OBTAINED_MODEM:
        printk("[TIME] Updated from MODEM\n");
        break;
    case DATE_TIME_OBTAINED_NTP:
        printk("[TIME] Updated from NTP\n");
        break;
    case DATE_TIME_NOT_OBTAINED:
        printk("[TIME] Update failed\n");
        break;
    default:
        break;
    }
}




void RTC_Start(void)
    {
        
        rtc = DEVICE_DT_GET(DT_NODELABEL(rtc0));
        if (!device_is_ready(rtc)) {
            LOG_ERR("RTC device is not ready");
            return;
        }
        
        int err = date_time_update_async(dt_evt_handler);
        if (err) {
            LOG_ERR("Failed to update date and time asynchronously: %d", err);
            return;
        } 
        printk("... \r\n");
        printk("... \r\n");
        k_msleep(GAP);
    }



    /* Voltage */
    static const struct adc_dt_spec adc_channel = ADC_DT_SPEC_GET(DT_PATH(zephyr_user));
    int16_t buf;
    int32_t val_mv;
    int err;
    float voltage = 0.0;

void Voltage_init(void)
{
    

    if (!adc_is_ready_dt(&adc_channel)) {
        LOG_ERR("ADC controller devivce %s not ready", adc_channel.dev->name);
        return 0;
    }

    err = adc_channel_setup_dt(&adc_channel);

        if (err < 0) {
            LOG_ERR("Could not setup channel #%d (%d)", 0, err);
            return 0;
        }

     	
	struct adc_sequence sequence = {
		.buffer = &buf,
		.buffer_size = sizeof(buf),
		
	};

    

    err = adc_sequence_init_dt(&adc_channel, &sequence);
	if (err < 0) {
		LOG_ERR("Could not initalize sequnce");
		return 0;
	}

    err = adc_read(adc_channel.dev, &sequence);
    if (err < 0) {
        LOG_ERR("Could not read (%d)", err);
    }

    val_mv = buf;

    err = adc_raw_to_millivolts_dt(&adc_channel, &val_mv);
    voltage = (float)val_mv / 1000.0;
    if (err < 0) {
                LOG_WRN(" (value in mV not available)\n");
        } 
}


    /* FLASH MEMORY SPI */
    
    #define FLASH_SIZE   (32 * 1024 * 1024)        //256Mbit = 32Mbyte    
    
    #define LOG_OFFFSET_FLASH_SIZE ( 1024 * 1024 * 11 ) // 11MB

    #define LOG_OFFSET_A         0x00000000 // 0MB
    #define LOG_OFFSET_B         0x00C00000 // 12MB
    

    char payload[512] = {0};

    static uint32_t write_offset_A = LOG_OFFSET_A;
    static uint32_t write_offset_B = LOG_OFFSET_B;
    static uint32_t write_offset_GNSS = LOG_OFFSET_GNSS;
    static uint32_t write_offset_Flag = GNSS_FLAG_OFFSET;
    



void save_log_to_flash(const char *buf , size_t len, uint32_t *write_offset ,uint32_t offset)
    {

        int err;

        if (*write_offset + len >= LOG_OFFFSET_FLASH_SIZE) {
        printk("Flash full -> wrap-around to 0\n");
        *write_offset = offset;
        }

        /* Sector erase when write_offset is at the beginning of a sector */ 
        if ((*write_offset % SECTOR_SIZE) == 0) {
            flash_erase(flash_dev, *write_offset, SECTOR_SIZE);
        }

        time_convert();

        err = flash_write(flash_dev, *write_offset , buf , len);
        if (err){
            printk("[FLASH] Flash write success addr: 0x%08x data: %s \r\n", *write_offset, buf);
        }else if(err < 0){
            LOG_ERR("[FLASH] Flash write failed %d \r\n", err);
        }
    

        *write_offset += len;

        printk("[FLASH] Data saved to flash,Once Measure is done\r\n %s\r\n", buf);
        printk("... \r\n");
        k_msleep(GAP);
    }



    int Publish_sensor_data_Gnss_pressure(const char *device_id_str,  uint64_t now_ms
                        )
{   
    

        char GNSS_Read_Buffer[512] = {0};
        char final_json[768] = {0};

        err = flash_read(flash_dev, LOG_OFFSET_GNSS , GNSS_Read_Buffer , sizeof(GNSS_Read_Buffer));
        if (err == 0) {
        printk("[FLASH] Read OK\n");
    } else {
        LOG_ERR("[FLASH] Read FAIL %d\n", err);
        return err;
    }

for (int i = 0; i < sizeof(GNSS_Read_Buffer); i++) {
    if (GNSS_Read_Buffer[i] == '}') {
        GNSS_Read_Buffer[i + 1] = '\0';
        break;
    }
}
    printk("[Publish_sensor_data_Gnss_pressure] FLASH READ  read buffer = %s\n", GNSS_Read_Buffer);
    time_convert();
    printk("\n");
   
    int len = snprintf(final_json, sizeof(final_json),
        "{"
            "\"Timestamp\":\"%llu\","
            "\"gnss\": %s"
        "}",
        now_ms, GNSS_Read_Buffer
    );

    if (len < 0 || len >= sizeof(final_json)) {
        LOG_ERR("JSON overflow");
        return -1;
    }

   // size_t json_len = strlen(GNSS_Read_Buffer);
    printk("[PUBLISH] Final JSON = %s\n", final_json);

    char topic[64] = {0};

    uint8_t id[8];    
    hwinfo_get_device_id(id, sizeof(id));

    snprintf(topic,sizeof(topic) ,"example-device/GPS_Data/%02x%02x%02x",
       id[0], id[1], id[2]);

    return data_publish_satelite(&client,
                        MQTT_QOS_2_EXACTLY_ONCE,
                        (const uint8_t *)final_json,
                       strlen(final_json), topic);

}



int publish_sensor_data(const char *device_id_str, float pm1, float pm25, float pm4, float pm10,
                        float rh, float temp, float co2, float voltage,float hPa, uint32_t result_raw_NOx, uint32_t result_Raw_VOC, uint64_t now_ms)
{

    char payload[512] = {0};
    int len = snprintf(payload, sizeof(payload),
        "{"
            "\"ID\":\"%s_A\","
            "\"pm1\":%.2f,"
            "\"pm25\":%.2f,"
            "\"pm4\":%.2f,"
            "\"pm10\":%.2f,"
            "\"rh\":%.2f,"
            "\"temp\":%.2f,"
            "\"co2\":%.2f,"
            "\"voltage\":%.2f,"
            "\"Pressure\":%.2f,"
            "\"Raw_nox\":%u,"
            "\"Raw_voc\":%u,"
            "\"Timestamp\":\"%llu\""
            "}",
            device_id_str, pm1, pm25, pm4, pm10,
            rh, temp, co2,voltage , hPa,result_raw_NOx ,result_Raw_VOC, now_ms);
    char topic[64] = {0};

    uint8_t id[8];    
    hwinfo_get_device_id(id, sizeof(id));

    snprintf(topic,sizeof(topic) ,"example-device/Main_Data/%02x%02x%02x",
       id[0], id[1], id[2]);
   
   

    return data_publish(&client,
                        MQTT_QOS_2_EXACTLY_ONCE,
                        (const uint8_t *)payload,
                        len, topic);

}



int publish_sensor_data_B(const char *device_id_str, float pm1, float pm25, float pm4, float pm10,
                        float rh, float temp, float co2, float voltage,float hPa, uint32_t result_raw_NOx, uint32_t result_Raw_VOC, uint64_t now_ms)
{
    
    char payload[512] = {0};
    int len = snprintf(payload, sizeof(payload),
        "{"
            "\"ID\":\"%s_B\","
            "\"pm1\":%.2f,"
            "\"pm25\":%.2f,"
            "\"pm4\":%.2f,"
            "\"pm10\":%.2f,"
            "\"rh\":%.2f,"
            "\"temp\":%.2f,"
            "\"co2\":%.2f,"
            "\"voltage\":%.2f,"
            "\"Pressure\":%.2f,"
            "\"Raw_nox\":%u,"
            "\"Raw_voc\":%u,"
            "\"Timestamp\":\"%llu\""
            "}",
            device_id_str, pm1, pm25, pm4, pm10,
            rh, temp, co2,voltage , hPa,result_raw_NOx ,result_Raw_VOC, now_ms);
    char topic[64] = {0};

    uint8_t id[8];    
    hwinfo_get_device_id(id, sizeof(id));

    snprintf(topic,sizeof(topic) ,"example-device/Main_Data/%02x%02x%02x_B",
       id[0], id[1], id[2]);
   
   

    return data_publish(&client,
                        MQTT_QOS_2_EXACTLY_ONCE,
                        (const uint8_t *)payload,
                        len, topic);

}

/* sum_avg_value */

uint64_t previous_time_set = 0;
volatile uint64_t divide = 0;


typedef struct {
    float pm1, pm25, pm4, pm10;
    float rh, temp, voltage, hPa;
    uint32_t co2;
    uint32_t raw_NOx, raw_VOC;
} SensorValues;

typedef struct {
    SensorValues current;   
    SensorValues prev;  
    SensorValues sum;       
    SensorValues result;    
    uint16_t sample_count;  
    uint16_t prev_sample_count; 
} SensorSet;

volatile SensorSet sensorA;
volatile SensorSet sensorB;





void sum_avg_value(SensorSet *Set, float pm1, float pm25, float pm4, float pm10,
                        float Humidity, float Temp, uint16_t co2, float voltage, float hPa,uint16_t Raw_NOx, uint16_t Raw_VOC)
{
    
    
    
    Set->sum.pm1 += pm1;
    Set->sum.pm25 += pm25;
    Set->sum.pm4 += pm4;
    Set->sum.pm10 += pm10;
    Set->sum.rh += Humidity;
    Set->sum.temp += Temp;
    Set->sum.co2 += co2;
    Set->sum.voltage += voltage;
    Set->sum.hPa += hPa;
    Set->sum.raw_NOx += Raw_NOx;
    Set->sum.raw_VOC += Raw_VOC;

    Set->sample_count++;
    printk("%d samples summed\r\n", Set->sample_count);

    Set->current.pm1 = Set->sum.pm1 / Set->sample_count;
    Set->current.pm25 = Set->sum.pm25 / Set->sample_count;
    Set->current.pm4 = Set->sum.pm4 / Set->sample_count;
    Set->current.pm10 = Set->sum.pm10 / Set->sample_count;
    Set->current.rh = Set->sum.rh / Set->sample_count;
    Set->current.temp = Set->sum.temp / Set->sample_count;
    Set->current.co2 = Set->sum.co2 / Set->sample_count;
    Set->current.voltage = Set->sum.voltage / Set->sample_count;
    Set->current.hPa = Set->sum.hPa / Set->sample_count;
    Set->current.raw_NOx = Set->sum.raw_NOx / Set->sample_count;
    Set->current.raw_VOC = Set->sum.raw_VOC / Set->sample_count;
    
        printk("[MongoDB] Store divided value to send datas to MongoDB\r\nPM1: %.2f, PM2.5: %.2f, PM4: %.2f, PM10: %.2f, RH: %.2f, Temp: %.2f, CO2: %d, Voltage: %.2f, Pressure: %.2f, Raw NOx %u , Raw VOC %u\r\n",
               Set->current.pm1, Set->current.pm25, Set->current.pm4, Set->current.pm10,
               Set->current.rh, Set->current.temp, 
               Set->current.co2, Set->current.voltage, Set->current.hPa , Set->current.raw_NOx, Set->current.raw_VOC);
    

}



void update_prev_value(SensorSet *Set)
{
    Set->prev.pm1 = Set->current.pm1;
    Set->prev.pm25 = Set->current.pm25;
    Set->prev.pm4 = Set->current.pm4;
    Set->prev.pm10 = Set->current.pm10;
    Set->prev.rh = Set->current.rh;
    Set->prev.temp = Set->current.temp;
    Set->prev.co2 = Set->current.co2;
    Set->prev.voltage = Set->current.voltage;
    Set->prev.hPa = Set->current.hPa;
    Set->prev.raw_NOx = Set->current.raw_NOx;
    Set->prev.raw_VOC = Set->current.raw_VOC;
  
}



void sum_prev_current_value(SensorSet *Set)
{
    Set->result.pm1 = ( Set->prev.pm1 + Set->current.pm1) / 2.0f;
    Set->result.pm25 = ( Set->prev.pm25 + Set->current.pm25) / 2.0f;
    Set->result.pm4 = ( Set->prev.pm4 + Set->current.pm4) / 2.0f;
    Set->result.pm10 = ( Set->prev.pm10 + Set->current.pm10) / 2.0f;
    Set->result.rh = ( Set->prev.rh + Set->current.rh) / 2.0f;
    Set->result.temp = ( Set->prev.temp + Set->current.temp) / 2.0f;
    Set->result.co2 = ( Set->prev.co2 + Set->current.co2) / 2.0f;
    Set->result.voltage = ( Set->prev.voltage + Set->current.voltage) / 2.0f;
    Set->result.hPa = ( Set->prev.hPa + Set->current.hPa) / 2.0f;
    Set->result.raw_NOx = ( Set->prev.raw_NOx + Set->current.raw_NOx) / 2;
    Set->result.raw_VOC = ( Set->prev.raw_VOC + Set->current.raw_VOC) / 2;
}




void First_loop_value(SensorSet *Set)
{
    Set->result.pm1 = Set->current.pm1;
    Set->result.pm25 = Set->current.pm25;
    Set->result.pm4 = Set->current.pm4;
    Set->result.pm10 = Set->current.pm10;
    Set->result.rh = Set->current.rh;
    Set->result.temp = Set->current.temp;
    Set->result.co2 = Set->current.co2;
    Set->result.voltage = Set->current.voltage;
    Set->result.hPa = Set->current.hPa;
    Set->result.raw_NOx = Set->current.raw_NOx;
    Set->result.raw_VOC = Set->current.raw_VOC;
}



void sum_value_reset(SensorSet *Set)
{
    if(Alarm_expired)
        {   
            
                Set->sum.pm1 = 0;
                Set->sum.pm25 = 0;
                Set->sum.pm4 = 0;
                Set->sum.pm10 = 0;
                Set->sum.rh = 0;
                Set->sum.temp = 0;
                Set->sum.co2 = 0;
                Set->sum.voltage = 0;
                Set->sum.hPa = 0;
                Set->sum.raw_NOx = 0;
                Set->sum.raw_VOC = 0;
                

                Set->prev_sample_count = Set->sample_count;
                Set->sample_count = 0;

                printk("sample_count %u,  prev_sample_count %u\r\n", Set->sample_count,Set->prev_sample_count );
            

    }
}



volatile int next_interval = 20;
bool static next_interval_locked = false;

#define EVERY_20_MINS 20
#define EVERY_5_MINS 5
#define EVERY_10_MINS 10

/* -------------------- MAIN CODE START POINT --------------------- */

void main(void)
{

    /* DUMP Station */
	int err;
	int ret;
    int i;

typedef struct{
    float    pm1_array[5];
    float    pm25_array[5];
    float    pm4_array[5];
    float    pm10_array[5];
    float    Humidity_array[5];
    float    Temp_array[5];
    float    NOx_raw_array[5];
    float    VOc_raw_array[5];
    uint16_t    co2_array[5];
}Array;

    Array Array_A = {0};
    Array Array_B = {0};

typedef struct{
    float    avg_pm1;
    volatile float    avg_pm25;
    float    avg_pm4;
    float    avg_pm10;
    float    avg_Humidity;
    float    avg_Temp;
    float avg_hPa;
    uint16_t    avg_NOx_raw;
    uint16_t    avg_VOc_raw;
    uint16_t   avg_co2;
}Average_values;

    Average_values Average_A = {0};
    Average_values Average_B = {0};

typedef struct{
    uint16_t data_buf[30];
    uint8_t buf[14];
}Data_buffer;

    Data_buffer Data_buffer_A = {0};
    Data_buffer Data_buffer_B = {0};

	uint32_t connect_attempt = 0;

    int count = 0;
    int pm25_flag = 0;
    int measure_interval_ms =0;

    static int over_count = 0;
    static int between_count = 0;
    static int under_count = 0;

    char buf[512] = {0};
    char buf_2[512] = {0};
    int len_1 ;
    int len_2 ;

    
    

    /* LED */
    gpio_pin_configure_dt(&led, GPIO_OUTPUT | GPIO_ACTIVE_HIGH);
    gpio_pin_set_dt(&led, 1);
    printk("LED ON \r\n");
        

    /* Device Id */
    device_id();

	printk("[SEN] Get information from SEN66 \r\n");

	k_sleep(K_MSEC(DEVICE_ID_WATTING_TIME));

	const struct i2c_dt_spec sen66 = I2C_DT_SPEC_GET(SEN66_NODE);
    const struct i2c_dt_spec sen66_1 = I2C_DT_SPEC_GET(SEN66_1_NODE);

	device_is_ready(sen66.bus);
	device_is_ready(sen66_1.bus);

    /* SEN66 Measurement Start */

	uint8_t cmd_start[] = {0x00, 0x21}; 

    ret = i2c_write_dt(&sen66, cmd_start, sizeof(cmd_start));
    if (ret == 0) {
        printk("[SEN] SEN66_A MEASUREMENT is starting \r\n");
    } else {
        printk("[SEN] SEN66_A MEASUREMENT IS FAILED : %d \r\n", ret);
        
    }
    k_msleep(50);

    ret = i2c_write_dt(&sen66_1, cmd_start, sizeof(cmd_start));
    if (ret == 0) {
        printk("[SEN] SEN66_B MEASUREMENT is starting \r\n");
    } else {
        printk("[SEN] SEN66_B MEASUREMENT IS FAILED : %d \r\n", ret);
        
    }
    printk("[SEN] Waiting for %dms SEN66 to be stable \r\n", STARTING_TIME);
    k_msleep(STARTING_TIME);

    printk("... \r\n");
    printk("... \r\n");
    


    /* BMP581 */

    int8_t rslt;
    int8_t chip_id;
    uint8_t i2c_addr = 0x46;
    uint8_t dummy = 0;

    i2c_dev = DEVICE_DT_GET(BMP581_NODE);
    if (!device_is_ready(i2c_dev)) {
        printk(" [BMP] BMP581 won't be started \r\n");
        return;
    }    printk("[BMP] BMP581 will be started \r\n");
 
    bmp_dev.intf = BMP5_I2C_INTF;
    bmp_dev.intf_ptr = (void *)i2c_dev;
    bmp_dev.read = user_i2c_read;
    bmp_dev.write = user_i2c_write;
    bmp_dev.delay_us = user_delay_us;

    k_msleep(2);

    if(i2c_dev == NULL) {
        printk("[BMP] i2c_dev is NULL\r\n");
    }


    /* To make the sensor stable */

    bmp5_get_regs(BMP5_REG_CHIP_ID, &dummy, 1, &bmp_dev);

     bmp5_get_regs(0x01, &chip_id,  1, &bmp_dev);
       printk("[BMP] BMP581 CHIP_ID: 0x%02X\n", chip_id);

    rslt = bmp5_init(&bmp_dev);
     if (rslt != BMP5_OK) {
         printk("[BMP] BMP5 init failed: %d\n", rslt);
     } 
   

    osr_cfg.press_en = BMP5_ENABLE;
    osr_cfg.osr_p = BMP5_OVERSAMPLING_4X; 
    osr_cfg.osr_t = BMP5_OVERSAMPLING_8X; // Temperature
    osr_cfg.odr = BMP5_ODR_50_HZ; // Output Data Rate


    rslt = bmp5_set_osr_odr_press_config(&osr_cfg, &bmp_dev);
    if (rslt != BMP5_OK) {
        printk(" Failed to get OSR/ODR config: %d\n", rslt);
        return;
    }

    struct bmp5_iir_config iir_cfg = {

    .set_iir_t = BMP5_IIR_FILTER_COEFF_3, // Temperature IIR
    .set_iir_p = BMP5_IIR_FILTER_COEFF_3, // Pressure IIR
    .shdw_set_iir_t = BMP5_ENABLE,
    .shdw_set_iir_p = BMP5_ENABLE,
    .iir_flush_forced_en = BMP5_ENABLE

    };

    rslt = bmp5_set_iir_config(&iir_cfg, &bmp_dev);
    if (rslt != BMP5_OK) {
        printk(" Failed to set IIR config: %d\n", rslt);
        return;
    }else {
        printk("[BMP] IIR config is set \r\n");
    }
    

    rslt = bmp5_set_power_mode(BMP5_POWERMODE_NORMAL, &bmp_dev);
    if (rslt != BMP5_OK) {
        printk(" Failed to set power mode: %d\n", rslt);
        return;
    }else {
        printk("[BMP] Power mode is Nomal \r\n");
    }

    printk("... \r\n");
    k_msleep(GAP);

    
    
    /* Watch dog start */
    wdt_init_and_start();
    
    /* Modem configuration */
    modem_configure();

    
    /* Gnss and LTE connection */
    printk(" GNSS Starts... \r\n");

    gnss_fix_once();

    k_sem_take(&gnss_done, K_FOREVER);
    
    wdt_try_feed();

    printk(" GNSS Done... \r\n");
    printk("...\r\n");
    printk("...\r\n");
    printk("[GNSS] Waiting for 20secs before LTE connection \r\n");
    k_sleep(K_SECONDS(20));


    /* LTE connection */    
    lte_connect_function();  

    gpio_pin_set_dt(&led, 0);
    k_sleep(K_SECONDS(5));
    printk("LED OFF \r\n");

    /* Setup PSM */
    PSM_eDRX();  
    k_sleep(K_SECONDS(2));

     /* RTC Start*/
    RTC_Start();

    /* MQTT Connect */
    MQTT_Reconnect();

   

        time_convert();

        err = Publish_sensor_data_Gnss_pressure(device_id_str, now_ms);
                            if (err){ 
                                printk("[MQTT] sending is failed: %d \r\n", err);
                            }
                            else { 
                                printk("[MQTT] Sending is success\r\n");
                                printk("...\r\n");
                            }                   

        

        printk("... \r\n");

    k_msleep(15000);
    printk("[BMP] Waitting for 15secs. \r\n");



    (void)mqtt_disconnect(&client, NULL);
    printk("[MQTT] MQTT Disconnected \r\n");

           

while (1) {
   

    printk("...\r\n");

    /* Watchdog Feed */
    wdt_try_feed();

    
    
    /* Sensor will be mesuring the datas */
    printk("[SEN] Start to read the measurement \r\n");
    printk("... \r\n");
    k_msleep(GAP);

    time_convert();
    /* SEN66 MEASUREMENT */
    SEN66_MEASURE_RAW_NOX_VOC(&sen66, Data_buffer_A.buf, sizeof(Data_buffer_A.buf),Array_A.VOc_raw_array, 6, "VOC_RAW", &Average_A.avg_VOc_raw);
    SEN66_MEASURE_RAW_NOX_VOC(&sen66, Data_buffer_A.buf, sizeof(Data_buffer_A.buf),Array_A.NOx_raw_array, 9, "NOX_RAW", &Average_A.avg_NOx_raw);
    SEN66_MEASURE_u16(&sen66, Data_buffer_A.data_buf, sizeof(Data_buffer_A.data_buf), Array_A.pm1_array, 0, "pm1", &Average_A.avg_pm1);
    SEN66_MEASURE_u16(&sen66, Data_buffer_A.data_buf, sizeof(Data_buffer_A.data_buf), Array_A.pm25_array, 3, "pm25", &Average_A.avg_pm25);
    SEN66_MEASURE_u16(&sen66, Data_buffer_A.data_buf, sizeof(Data_buffer_A.data_buf), Array_A.pm4_array, 6, "pm4", &Average_A.avg_pm4);
    SEN66_MEASURE_u16(&sen66, Data_buffer_A.data_buf, sizeof(Data_buffer_A.data_buf), Array_A.pm10_array, 9, "pm10", &Average_A.avg_pm10);
    SEN66_MEASURE_s16(&sen66, Data_buffer_A.data_buf, sizeof(Data_buffer_A.data_buf), Array_A.Humidity_array, 12, "Humidity", &Average_A.avg_Humidity, 100.0f, "%" ) ;
    SEN66_MEASURE_CO2(&sen66, Data_buffer_A.data_buf, sizeof(Data_buffer_A.data_buf), Array_A.co2_array, 24, "CO2", &Average_A.avg_co2);

     printk("[SEN_B] Start to read the measurement \r\n");
    printk("... \r\n");
        k_msleep(GAP);




    SEN66_MEASURE_RAW_NOX_VOC(&sen66_1, Data_buffer_B.buf, sizeof(Data_buffer_B.buf),Array_B.VOc_raw_array, 6, "VOC_RAW", &Average_B.avg_VOc_raw);
    SEN66_MEASURE_RAW_NOX_VOC(&sen66_1, Data_buffer_B.buf, sizeof(Data_buffer_B.buf),Array_B.NOx_raw_array, 9, "NOX_RAW", &Average_B.avg_NOx_raw);
    SEN66_MEASURE_u16(&sen66_1, Data_buffer_B.data_buf, sizeof(Data_buffer_B.data_buf), Array_B.pm1_array, 0, "pm1", &Average_B.avg_pm1);
    SEN66_MEASURE_u16(&sen66_1, Data_buffer_B.data_buf, sizeof(Data_buffer_B.data_buf), Array_B.pm25_array, 3, "pm25", &Average_B.avg_pm25);
    SEN66_MEASURE_u16(&sen66_1, Data_buffer_B.data_buf, sizeof(Data_buffer_B.data_buf), Array_B.pm4_array, 6, "pm4", &Average_B.avg_pm4);
    SEN66_MEASURE_u16(&sen66_1, Data_buffer_B.data_buf, sizeof(Data_buffer_B.data_buf), Array_B.pm10_array, 9, "pm10", &Average_B.avg_pm10);
    SEN66_MEASURE_s16(&sen66_1, Data_buffer_B.data_buf, sizeof(Data_buffer_B.data_buf), Array_B.Humidity_array, 12, "Humidity", &Average_B.avg_Humidity, 100.0f, "%" ) ;
    SEN66_MEASURE_CO2(&sen66_1, Data_buffer_B.data_buf, sizeof(Data_buffer_B.data_buf), Array_B.co2_array, 24, "CO2", &Average_B.avg_co2);
    time_convert();

    
    

    /* BMP581 Pressure(*4) and Temp(*8) */
    bmp5_get_sensor_data(&sensor_data, &osr_cfg, &bmp_dev);
    Average_A.avg_hPa = sensor_data.pressure/100;
    Average_B.avg_hPa = sensor_data.pressure/100;

    Average_A.avg_Temp = sensor_data.temperature;
    Average_B.avg_Temp = sensor_data.temperature;

    float temp = sensor_data.temperature;


    


    printk("\r\n");
    printk(" -- Average values -- \r\n");

    k_sleep(K_SECONDS(1));

	/* Print values */ 
    printk("[SEN_A] PM1.0     : %.2f ug/m3\r\n",Average_A.avg_pm1);
    printk("[SEN_A] PM2.5     : %.2f ug/m3\r\n",Average_A.avg_pm25);
    printk("[SEN_A] PM4.0     : %.2f ug/m3\r\n",Average_A.avg_pm4);
    printk("[SEN_A] PM10.0    : %.2f ug/m3\r\n",Average_A.avg_pm10);
    printk("[SEN_A] RH [%%]    : %.2f %%\r\n",Average_A.avg_Humidity);
    printk("[SEN_A] T[C]      : %.2f T[C]\r\n" ,Average_A.avg_Temp);
    printk("[SEN_A] CO2       : %u   ppm\r\n",Average_A.avg_co2); 
    printk("[BMP_A] hPa       : %.2f hPa\r\n", Average_A.avg_hPa);
    printk("[SEN_A] VOC RAW Data : %u ticks\r\n", Average_A.avg_VOc_raw);
    printk("[SEN_A] NOx RAW Data : %u ticks\r\n", Average_A.avg_NOx_raw);
    printk("\r\n");
    printk("\r\n");
    printk("[SEN_B] PM1.0     : %.2f ug/m3\r\n",Average_B.avg_pm1);
    printk("[SEN_B] PM2.5     : %.2f ug/m3\r\n",Average_B.avg_pm25);
    printk("[SEN_B] PM4.0     : %.2f ug/m3\r\n",Average_B.avg_pm4);
    printk("[SEN_B] PM10.0    : %.2f ug/m3\r\n",Average_B.avg_pm10);
    printk("[SEN_B] RH [%%]    : %.2f %%\r\n",Average_B.avg_Humidity);
    printk("[SEN_B] T[C]      : %.2f T[C]\r\n" ,Average_B.avg_Temp);
    printk("[SEN_B] CO2       : %u   ppm\r\n",Average_B.avg_co2); 
    printk("[BMP_B] hPa       : %.2f hPa\r\n", Average_B.avg_hPa);
    printk("[SEN_B] VOC RAW Data : %u ticks\r\n", Average_B.avg_VOc_raw);
    printk("[SEN_B] NOx RAW Data : %u ticks\r\n", Average_B.avg_NOx_raw);

    /* Voltage Measurement */
    Voltage_init();
    printk("[ADC] VDD       : %.2f V \r\n", voltage);

    printk("\r\n");
    printk(" -- Measurements are done -- \r\n");
    printk("\r\n");
	

    

    /* Store data into Flash Memory */
    len_1 = snprintf(buf, sizeof(buf),
        "%.2f,%.2f,%.2f,%.2f,"
        "%.2f,%.2f,%.2f,"
        "%u,%u,%u\r\n%llu",
        Average_A.avg_pm1,
        Average_A.avg_pm25,
        Average_A.avg_pm4,
        Average_A.avg_pm10,
        Average_A.avg_Humidity,
        Average_A.avg_Temp,
        Average_A.avg_hPa,
        Average_A.avg_NOx_raw,
        Average_A.avg_VOc_raw,
        Average_A.avg_co2,
        now_ms
        );



    len_2 = snprintf(buf_2, sizeof(buf_2),
        "%.2f,%.2f,%.2f,%.2f,"
        "%.2f,%.2f,%.2f,"
        "%u,%u,%u\r\n%llu",
        Average_B.avg_pm1,
        Average_B.avg_pm25,
        Average_B.avg_pm4,
        Average_B.avg_pm10,
        Average_B.avg_Humidity,
        Average_B.avg_Temp,
        Average_B.avg_hPa,
        Average_B.avg_NOx_raw,
        Average_B.avg_VOc_raw,
        Average_B.avg_co2,
        now_ms
        );

    save_log_to_flash(buf, len_1, &write_offset_A, LOG_OFFSET_A);

    k_sleep(K_SECONDS(1));

    save_log_to_flash(buf_2, len_2, &write_offset_B, LOG_OFFSET_B);

    sum_avg_value(&sensorA, Average_A.avg_pm1, Average_A.avg_pm25, Average_A.avg_pm4, Average_A.avg_pm10, Average_A.avg_Humidity, Average_A.avg_Temp, Average_A.avg_co2, voltage, Average_A.avg_hPa, Average_A.avg_NOx_raw, Average_A.avg_VOc_raw); // current 

    sum_avg_value(&sensorB ,Average_B.avg_pm1, Average_B.avg_pm25, Average_B.avg_pm4, Average_B.avg_pm10, Average_B.avg_Humidity, Average_B.avg_Temp, Average_B.avg_co2, voltage, Average_B.avg_hPa, Average_B.avg_NOx_raw, Average_B.avg_VOc_raw);


    if(!next_interval_locked)
    {
        if(Average_A.avg_pm25 >= 35.5f && Average_B.avg_pm25 >= 35.5f) // >35.5f
            {   
                if(next_interval >= EVERY_5_MINS)
                {
                    //printk("[Current pm25] currnet_pm25 over 0.5 %.2f \r\n", avg_pm25);
                    next_interval = EVERY_5_MINS;
                    next_interval_locked = true;
                    printk("[Currnet pm25] next_interval is set 5\r\n");
                }            
                
            }else if ( Average_A.avg_pm25 < 35.5f && Average_A.avg_pm25 >= 12.1f && Average_B.avg_pm25 < 35.5f && Average_B.avg_pm25 >= 12.1f ) //35.5f > > 12.1f
            {
                if(next_interval >= EVERY_10_MINS)
                {
                    
                    next_interval = EVERY_10_MINS;
                    printk("[Currnet pm25] next_interval is set 10\r\n");
                }
                
            }

    }


    printk("[Next interval] Time has been changed: %d\r\n", next_interval);


    k_msleep(1000); 

    /* Watchdog Feed */
    wdt_try_feed();


    }
}

/* Multi Thread */
int Measurement = 20;
int count = 0;

void publishing_mqtt_data_thread(void)
{
    
    while(1)
    {     
        k_sem_take(&alarm_sem, K_FOREVER);
                 
        if(Alarm_expired)
        {

            Measurement = next_interval;
            printk("next interval was set Before Alarm expired next_interval:%d\r\n",next_interval);
            count++;

            if(count >= 3)
            {
                date_time_update_async(date_time_evt_handler);
                count = 0;
            }

            reset_rtc_alarm(now_ms, next_interval);

            time_convert();  

            k_msleep(2000);

            switch(Measurement)
            {
                case EVERY_20_MINS:
                    printk("[Every 20 Mins] publish datas \r\n");
                    Alarm_expired = false;


                    /* MQTT Reconnect */
                    MQTT_Reconnect();

                    if(sensorA.prev_sample_count > 0)
                    {
                        sum_prev_current_value(&sensorA);
                        sum_prev_current_value(&sensorB);

                    }else{
                        First_loop_value(&sensorA);
                        First_loop_value(&sensorB);

                    }  

                    time_convert();

                    publish_sensor_data(device_id_str, sensorA.result.pm1,sensorA.result.pm25,sensorA.result.pm4,sensorA.result.pm10,sensorA.result.rh,sensorA.result.temp,sensorA.result.co2,sensorA.result.voltage,sensorA.result.hPa, sensorA.result.raw_NOx, sensorA.result.raw_VOC,now_ms);
                    k_sleep(K_SECONDS(1)); 

                    publish_sensor_data_B(device_id_str, sensorB.result.pm1,sensorB.result.pm25,sensorB.result.pm4,sensorB.result.pm10,sensorB.result.rh,sensorB.result.temp,sensorB.result.co2,sensorB.result.voltage,sensorB.result.hPa, sensorB.result.raw_NOx, sensorB.result.raw_VOC,now_ms);

                    update_prev_value(&sensorA);
                    update_prev_value(&sensorB);

               
                    
                    sum_value_reset(&sensorA);
                    sum_value_reset(&sensorB);

                    k_sleep(K_SECONDS(1)); 

                    (void)mqtt_disconnect(&client, NULL);
                    
                    

                    
                    
                    next_interval_locked = false;
                    next_interval = EVERY_20_MINS;

                    
                    
                    break;

                case EVERY_5_MINS:
                printk("[Every 5 Mins] publish datas\r\n");
                    Alarm_expired = false;
                
                  /* MQTT Reconnect */
                    MQTT_Reconnect();

                    if(sensorA.prev_sample_count > 0)
                    {
                        sum_prev_current_value(&sensorA);
                        sum_prev_current_value(&sensorB);

                    }else{
                        First_loop_value(&sensorA);
                        First_loop_value(&sensorB);

                    }  

                    time_convert();

                    publish_sensor_data(device_id_str, sensorA.result.pm1,sensorA.result.pm25,sensorA.result.pm4,sensorA.result.pm10,sensorA.result.rh,sensorA.result.temp,sensorA.result.co2,sensorA.result.voltage,sensorA.result.hPa, sensorA.result.raw_NOx, sensorA.result.raw_VOC,now_ms);

                    k_sleep(K_SECONDS(1)); 

                    publish_sensor_data_B(device_id_str, sensorB.result.pm1,sensorB.result.pm25,sensorB.result.pm4,sensorB.result.pm10,sensorB.result.rh,sensorB.result.temp,sensorB.result.co2,sensorB.result.voltage,sensorB.result.hPa, sensorB.result.raw_NOx, sensorB.result.raw_VOC,now_ms);

                    update_prev_value(&sensorA);
                    update_prev_value(&sensorB);
                   

                
                    sum_value_reset(&sensorA);
                    sum_value_reset(&sensorB);

                    k_sleep(K_SECONDS(1)); 
             
    
                    (void)mqtt_disconnect(&client, NULL);
      

                    
                    next_interval_locked = false;
                    next_interval = EVERY_20_MINS;

                    break;

                case EVERY_10_MINS:

                    printk("[Every 10 Mins] publish datas \r\n");
                    
                    Alarm_expired = false;


                 /* MQTT Reconnect */
                    MQTT_Reconnect();

                    if(sensorA.prev_sample_count > 0)
                    {
                        sum_prev_current_value(&sensorA);
                        sum_prev_current_value(&sensorB);

                    }else{
                        First_loop_value(&sensorA);
                        First_loop_value(&sensorB);

                    }  

                    time_convert();

                     publish_sensor_data(device_id_str, sensorA.result.pm1,sensorA.result.pm25,sensorA.result.pm4,sensorA.result.pm10,sensorA.result.rh,sensorA.result.temp,sensorA.result.co2,sensorA.result.voltage,sensorA.result.hPa, sensorA.result.raw_NOx, sensorA.result.raw_VOC,now_ms);

                    k_sleep(K_SECONDS(1)); 

                    publish_sensor_data_B(device_id_str, sensorB.result.pm1,sensorB.result.pm25,sensorB.result.pm4,sensorB.result.pm10,sensorB.result.rh,sensorB.result.temp,sensorB.result.co2,sensorB.result.voltage,sensorB.result.hPa, sensorB.result.raw_NOx, sensorB.result.raw_VOC,now_ms);
                    

                    update_prev_value(&sensorA);
                    update_prev_value(&sensorB);
                    

          
                    
                    sum_value_reset(&sensorA);
                    sum_value_reset(&sensorB);

                    k_sleep(K_SECONDS(1)); 
                    
                        (void)mqtt_disconnect(&client, NULL);
            
             


                    next_interval_locked = false;
                    next_interval = EVERY_20_MINS;

                    
                    break;
            }
        
        }

        sensorA.prev.pm25 = sensorA.current.pm25;
        sensorB.prev.pm25 = sensorB.current.pm25;

    }

    }

K_THREAD_DEFINE(thread0_id, STACK_SIZE, publishing_mqtt_data_thread, NULL, NULL, NULL, THREAD_PRIORITY, 0, 0);


