// **********************************
// * Settings                       *
// **********************************

// * Update treshold in milliseconds, messages will only be sent on this interval
#define UPDATE_INTERVAL 60000  // 1 minute
//#define UPDATE_INTERVAL 300000 // 5 minutes

// * Baud rate for both hardware and software
#define BAUD_RATE 115200

// The used serial pins, note that this can only be UART0, as other serial port doesn't support inversion
// By default the UART0 serial will be used. These settings displayed here just as a reference.
// #define SERIAL_RX RX
// #define SERIAL_TX TX

// * Size of the hardware UART receive buffer.
// * A DSMR5 telegram is ~1kb and arrives every second at 115200 baud. A bigger
// * buffer means a slow loop iteration (webserver request, OTA, mqtt reconnect)
// * can no longer corrupt the telegram that is being received at that moment.
#define SERIAL_RX_BUFFER_SIZE 1024

// * Max telegram length
#define P1_MAXLINELENGTH 1050

// * Set to 1 to echo every received telegram line back over the serial TX line.
// * Handy while debugging the wiring, but it is a lot of blocking serial writes.
#define DEBUG_TELEGRAM 0

// * The hostname of our little creature
#define HOSTNAME "p1meter"

// * The password used for OTA
#define OTA_PASSWORD "admin"

// * Credentials for the web configuration interface (http://p1meter.local/)
// * These are only the defaults for a virgin device, they can be changed at
// * runtime on the settings page and are then stored in EEPROM.
#define WEB_USERNAME "admin"
#define WEB_PASSWORD "p1meter"

// * Port for the web configuration interface
#define WEB_PORT 80

// * Wifi timeout in milliseconds
#define WIFI_TIMEOUT 30000

// * MQTT root topic
#define MQTT_ROOT_TOPIC "sensors/power/p1meter"

// * Availability (Last Will & Testament) topic and payloads.
// * "online" is published (retained) after every succesful connect, the broker
// * publishes "offline" on our behalf as soon as we drop off the network.
#define MQTT_STATUS_TOPIC MQTT_ROOT_TOPIC "/status"
#define MQTT_STATUS_ONLINE "online"
#define MQTT_STATUS_OFFLINE "offline"

// * Reconnecting to mqtt is non blocking and backs off between these bounds
#define MQTT_RECONNECT_MIN_INTERVAL 2000    // * 2 seconds
#define MQTT_RECONNECT_MAX_INTERVAL 60000   // * 1 minute

// * Retry a failed publish this quickly instead of waiting a full UPDATE_INTERVAL
#define MQTT_PUBLISH_RETRY_INTERVAL 5000

// * Force a fresh connection after this many consecutive failed publishes.
// * Catches the case where PubSubClient still reports connected() while the
// * underlying tcp socket is already dead (broker restarted underneath us).
#define MQTT_MAX_PUBLISH_FAILURES 3

// * Last resort: reboot when mqtt / wifi have been unreachable for this long
#define MQTT_FAILSAFE_TIMEOUT 300000  // * 5 minutes
#define WIFI_FAILSAFE_TIMEOUT 300000  // * 5 minutes

// * A telegram older than this means the meter itself went quiet, which is not
// * something a reboot is going to fix
#define P1_TELEGRAM_STALE_TIMEOUT 60000  // * 1 minute

// * EEPROM layout marker. Bump this when the ConfigData struct changes so
// * stale settings from an older firmware are ignored instead of misread.
#define CONFIG_MAGIC 0x50314D02  // * "P1M" + layout version

// * Settings stored in EEPROM. Everything in here is editable at runtime.
struct ConfigData
{
    uint32_t magic;
    char mqtt_host[64];
    char mqtt_port[6];
    char mqtt_user[32];
    char mqtt_pass[32];
    char web_user[32];
    char web_pass[32];
    uint16_t crc;
};

// * Live settings. Filled with the defaults below, overwritten by EEPROM on
// * boot (when valid) and by the web interface / WiFiManager portal at runtime.
char MQTT_HOST[64] = "";
char MQTT_PORT[6]  = "1883";
char MQTT_USER[32] = "";
char MQTT_PASS[32] = "";
char WEB_USER[32]  = WEB_USERNAME;
char WEB_PASS[32]  = WEB_PASSWORD;

// * Unique mqtt client id, built from the hostname and the mac address in setup()
char MQTT_CLIENT_ID[32] = HOSTNAME;

// **********************************
// * Runtime state                  *
// **********************************

// * All timers are unsigned so millis() rollover math keeps working
unsigned long LAST_UPDATE_SENT        = 0;  // * last succesful publish of a full set
unsigned long LAST_PUBLISH_ATTEMPT    = 0;
unsigned long LAST_RECONNECT_ATTEMPT  = 0;
unsigned long LAST_MQTT_CONNECTED     = 0;
unsigned long LAST_WIFI_CONNECTED     = 0;
unsigned long LAST_TELEGRAM_RECEIVED  = 0;  // * last telegram that passed the crc check
unsigned long MQTT_RECONNECT_INTERVAL = MQTT_RECONNECT_MIN_INTERVAL;
unsigned int  MQTT_RECONNECT_ATTEMPTS = 0;  // * consecutive failed connects
unsigned int  MQTT_PUBLISH_FAILURES   = 0;  // * consecutive failed publishes
int           MQTT_LAST_STATE         = 0;  // * PubSubClient state() of the last attempt

// * True once at least one telegram passed the crc check, so we never publish
// * a set of zeroes right after boot
bool DATA_READY = false;

// * True when the last publish attempt failed and the cached values still
// * need to go out to the broker
bool PUBLISH_PENDING = false;

// * Scheduled reboot (0 = none), used so web handlers never block on a restart
unsigned long REBOOT_AT = 0;

// * Set to store received telegram
char telegram[P1_MAXLINELENGTH];

// * Set to store the data values read
long CONSUMPTION_LOW_TARIF;
long CONSUMPTION_HIGH_TARIF;

long RETURNDELIVERY_LOW_TARIF;
long RETURNDELIVERY_HIGH_TARIF;

long ACTUAL_CONSUMPTION;
long ACTUAL_RETURNDELIVERY;
long GAS_METER_M3;

long L1_INSTANT_POWER_USAGE;
long L2_INSTANT_POWER_USAGE;
long L3_INSTANT_POWER_USAGE;
long L1_INSTANT_POWER_CURRENT;
long L2_INSTANT_POWER_CURRENT;
long L3_INSTANT_POWER_CURRENT;
long L1_VOLTAGE;
long L2_VOLTAGE;
long L3_VOLTAGE;

// Set to store data counters read
long ACTUAL_TARIF;
long SHORT_POWER_OUTAGES;
long LONG_POWER_OUTAGES;
long SHORT_POWER_DROPS;
long SHORT_POWER_PEAKS;

// * Set during CRC checking
unsigned int currentCRC = 0;
