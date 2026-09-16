#include <FS.h>
#include <EEPROM.h>
#include <DNSServer.h>
#include <ESP8266WiFi.h>
#include <Ticker.h>
#include <WiFiManager.h>
#include <ESP8266mDNS.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>

// * Include settings
#include "settings.h"
#include "webpages.h"

// * Initiate led blinker library
Ticker ticker;

// * Initiate WIFI client
WiFiClient espClient;

// * Initiate MQTT client
PubSubClient mqtt_client(espClient);

// * Initiate the web configuration interface
ESP8266WebServer server(WEB_PORT);

// * Forward declarations (so the order of this file stays readable)
unsigned int CRC16(unsigned int crc, unsigned char *buf, int len);
void processLine(int len);
void apply_mqtt_settings();
bool mqtt_connect_attempt();

// **********************************
// * WIFI                           *
// **********************************

// * Gets called when WiFiManager enters configuration mode
void configModeCallback(WiFiManager *myWiFiManager)
{
    Serial.println(F("Entered config mode"));
    Serial.println(WiFi.softAPIP());

    // * If you used auto generated SSID, print it
    Serial.println(myWiFiManager->getConfigPortalSSID());

    // * Entered config mode, make led toggle faster
    ticker.attach(0.2, tick);
}

// **********************************
// * Ticker (System LED Blinker)    *
// **********************************

// * Blink on-board Led
void tick()
{
    // * Toggle state
    int state = digitalRead(LED_BUILTIN);    // * Get the current state of GPIO1 pin
    digitalWrite(LED_BUILTIN, !state);       // * Set pin to the opposite state
}

// **********************************
// * EEPROM helpers                 *
// **********************************

// * Checksum over everything in the struct except the checksum itself
uint16_t config_checksum(const ConfigData &config)
{
    return (uint16_t) CRC16(0x0000, (unsigned char *) &config, sizeof(ConfigData) - sizeof(config.crc));
}

// * Copy the live settings into a struct that can go into EEPROM
void config_from_globals(ConfigData &config)
{
    memset(&config, 0, sizeof(ConfigData));

    config.magic = CONFIG_MAGIC;
    strlcpy(config.mqtt_host, MQTT_HOST, sizeof(config.mqtt_host));
    strlcpy(config.mqtt_port, MQTT_PORT, sizeof(config.mqtt_port));
    strlcpy(config.mqtt_user, MQTT_USER, sizeof(config.mqtt_user));
    strlcpy(config.mqtt_pass, MQTT_PASS, sizeof(config.mqtt_pass));
    strlcpy(config.web_user,  WEB_USER,  sizeof(config.web_user));
    strlcpy(config.web_pass,  WEB_PASS,  sizeof(config.web_pass));
    config.crc = config_checksum(config);
}

// * Read the stored settings, falling back to the defaults in settings.h
bool load_config()
{
    ConfigData config;
    EEPROM.get(0, config);

    if (config.magic != CONFIG_MAGIC)
    {
        Serial.println(F("EEPROM: no stored settings found, using defaults from settings.h"));
        return false;
    }

    if (config.crc != config_checksum(config))
    {
        Serial.println(F("EEPROM: stored settings are corrupt, using defaults from settings.h"));
        return false;
    }

    // * Make sure a truncated string in EEPROM can never run off the end
    config.mqtt_host[sizeof(config.mqtt_host) - 1] = 0;
    config.mqtt_port[sizeof(config.mqtt_port) - 1] = 0;
    config.mqtt_user[sizeof(config.mqtt_user) - 1] = 0;
    config.mqtt_pass[sizeof(config.mqtt_pass) - 1] = 0;
    config.web_user[sizeof(config.web_user) - 1]   = 0;
    config.web_pass[sizeof(config.web_pass) - 1]   = 0;

    strlcpy(MQTT_HOST, config.mqtt_host, sizeof(MQTT_HOST));
    strlcpy(MQTT_PORT, config.mqtt_port, sizeof(MQTT_PORT));
    strlcpy(MQTT_USER, config.mqtt_user, sizeof(MQTT_USER));
    strlcpy(MQTT_PASS, config.mqtt_pass, sizeof(MQTT_PASS));

    // * Only adopt stored web credentials when they are actually set
    if (strlen(config.web_user) > 0)
        strlcpy(WEB_USER, config.web_user, sizeof(WEB_USER));

    if (strlen(config.web_pass) > 0)
        strlcpy(WEB_PASS, config.web_pass, sizeof(WEB_PASS));

    Serial.println(F("EEPROM: stored settings loaded"));
    return true;
}

// * Persist the live settings
bool save_config()
{
    ConfigData config;
    config_from_globals(config);

    EEPROM.put(0, config);
    bool result = EEPROM.commit();

    Serial.println(result ? F("EEPROM: settings saved") : F("EEPROM: saving settings failed"));
    return result;
}

// **********************************
// * MQTT                           *
// **********************************

// * Send a message to a broker topic
bool send_mqtt_message(const char *topic, const char *payload)
{
    bool result = mqtt_client.publish(topic, payload, false);

    if (!result)
        Serial.printf("MQTT publish to topic %s failed\n", topic);

    return result;
}

// * Publish our availability so Home Assistant can show the device as
// * unavailable instead of silently keeping the last known values
void publish_availability()
{
    mqtt_client.publish(MQTT_STATUS_TOPIC, MQTT_STATUS_ONLINE, true);
}

// * One non blocking connection attempt. Never call this in a loop, loop()
// * decides when the next attempt is due.
bool mqtt_connect_attempt()
{
    LAST_RECONNECT_ATTEMPT = millis();
    MQTT_RECONNECT_ATTEMPTS++;

    Serial.printf("MQTT connection attempt %u to %s:%s as %s ...\n",
                  MQTT_RECONNECT_ATTEMPTS, MQTT_HOST, MQTT_PORT, MQTT_CLIENT_ID);

    // * Register a Last Will so the broker announces us as offline the moment
    // * we drop off the network
    bool connected;

    if (strlen(MQTT_USER) > 0)
        connected = mqtt_client.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS,
                                        MQTT_STATUS_TOPIC, 0, true, MQTT_STATUS_OFFLINE);
    else
        connected = mqtt_client.connect(MQTT_CLIENT_ID, NULL, NULL,
                                        MQTT_STATUS_TOPIC, 0, true, MQTT_STATUS_OFFLINE);

    MQTT_LAST_STATE = mqtt_client.state();

    if (!connected)
    {
        // * Back off a bit further on every failure, capped at the maximum
        unsigned long next_interval = MQTT_RECONNECT_INTERVAL * 2;

        if (next_interval > MQTT_RECONNECT_MAX_INTERVAL)
            next_interval = MQTT_RECONNECT_MAX_INTERVAL;

        MQTT_RECONNECT_INTERVAL = next_interval;

        Serial.printf("MQTT connection failed: rc=%d, next attempt in %lu ms\n",
                      MQTT_LAST_STATE, MQTT_RECONNECT_INTERVAL);
        return false;
    }

    Serial.println(F("MQTT connected!"));

    MQTT_RECONNECT_ATTEMPTS = 0;
    MQTT_PUBLISH_FAILURES   = 0;
    MQTT_RECONNECT_INTERVAL = MQTT_RECONNECT_MIN_INTERVAL;
    LAST_MQTT_CONNECTED     = millis();

    publish_availability();

    // * Keep the original startup announcement
    char message[24 + sizeof(MQTT_CLIENT_ID)];
    snprintf(message, sizeof(message), "p1 meter alive: %s", MQTT_CLIENT_ID);
    mqtt_client.publish("hass/status", message);

    Serial.printf("MQTT root topic: %s\n", MQTT_ROOT_TOPIC);

    // * Push the cached readings out right away instead of waiting for the
    // * next update interval, so Home Assistant recovers immediately
    if (DATA_READY)
        PUBLISH_PENDING = true;

    return true;
}

bool send_metric(const char *name, long metric)
{
    char payload[16];
    ltoa(metric, payload, 10);

    char topic[96];
    snprintf(topic, sizeof(topic), "%s/%s", MQTT_ROOT_TOPIC, name);

    return send_mqtt_message(topic, payload);
}

// * Publish the cached values. Stops at the first failure, the caller keeps
// * the data around and retries.
bool send_data_to_broker()
{
    bool ok = true;

    ok = ok && send_metric("consumption_low_tarif", CONSUMPTION_LOW_TARIF);
    ok = ok && send_metric("consumption_high_tarif", CONSUMPTION_HIGH_TARIF);
    ok = ok && send_metric("returndelivery_low_tarif", RETURNDELIVERY_LOW_TARIF);
    ok = ok && send_metric("returndelivery_high_tarif", RETURNDELIVERY_HIGH_TARIF);
    ok = ok && send_metric("actual_consumption", ACTUAL_CONSUMPTION);
    ok = ok && send_metric("actual_returndelivery", ACTUAL_RETURNDELIVERY);

    ok = ok && send_metric("l1_instant_power_usage", L1_INSTANT_POWER_USAGE);
    ok = ok && send_metric("l2_instant_power_usage", L2_INSTANT_POWER_USAGE);
    ok = ok && send_metric("l3_instant_power_usage", L3_INSTANT_POWER_USAGE);
    ok = ok && send_metric("l1_instant_power_current", L1_INSTANT_POWER_CURRENT);
    ok = ok && send_metric("l2_instant_power_current", L2_INSTANT_POWER_CURRENT);
    ok = ok && send_metric("l3_instant_power_current", L3_INSTANT_POWER_CURRENT);
    ok = ok && send_metric("l1_voltage", L1_VOLTAGE);
    ok = ok && send_metric("l2_voltage", L2_VOLTAGE);
    ok = ok && send_metric("l3_voltage", L3_VOLTAGE);

    ok = ok && send_metric("gas_meter_m3", GAS_METER_M3);

    ok = ok && send_metric("actual_tarif_group", ACTUAL_TARIF);
    ok = ok && send_metric("short_power_outages", SHORT_POWER_OUTAGES);
    ok = ok && send_metric("long_power_outages", LONG_POWER_OUTAGES);
    ok = ok && send_metric("short_power_drops", SHORT_POWER_DROPS);
    ok = ok && send_metric("short_power_peaks", SHORT_POWER_PEAKS);

    return ok;
}

// * Apply (possibly changed) mqtt settings without needing a reboot
void apply_mqtt_settings()
{
    if (mqtt_client.connected())
    {
        mqtt_client.publish(MQTT_STATUS_TOPIC, MQTT_STATUS_OFFLINE, true);
        mqtt_client.disconnect();
    }

    mqtt_client.setServer(MQTT_HOST, atoi(MQTT_PORT));

    // * Reset the backoff so the new settings are tried immediately
    MQTT_RECONNECT_INTERVAL = MQTT_RECONNECT_MIN_INTERVAL;
    MQTT_RECONNECT_ATTEMPTS = 0;
    MQTT_PUBLISH_FAILURES   = 0;
    LAST_RECONNECT_ATTEMPT  = millis() - MQTT_RECONNECT_INTERVAL;

    // * Give the new settings a full failsafe window before we reboot on them
    LAST_MQTT_CONNECTED = millis();

    Serial.printf("MQTT target set to %s:%s\n", MQTT_HOST, MQTT_PORT);
}

// * Keep the mqtt connection alive. Returns without blocking, always.
void handle_mqtt()
{
    unsigned long now = millis();

    if (mqtt_client.connected())
    {
        LAST_MQTT_CONNECTED = now;
        mqtt_client.loop();
        return;
    }

    // * No point in hammering a broker we cannot reach anyway
    if (WiFi.status() != WL_CONNECTED)
        return;

    if (now - LAST_RECONNECT_ATTEMPT >= MQTT_RECONNECT_INTERVAL)
        mqtt_connect_attempt();
}

// * Decide whether the cached readings are due to be published
bool publish_due()
{
    if (!DATA_READY)
        return false;

    unsigned long now = millis();

    // * A failed publish is retried quickly instead of dropping the data
    if (PUBLISH_PENDING)
        return (now - LAST_PUBLISH_ATTEMPT >= MQTT_PUBLISH_RETRY_INTERVAL);

    return (now - LAST_UPDATE_SENT >= UPDATE_INTERVAL);
}

void handle_publish()
{
    if (!mqtt_client.connected() || !publish_due())
        return;

    LAST_PUBLISH_ATTEMPT = millis();

    if (send_data_to_broker())
    {
        LAST_UPDATE_SENT      = LAST_PUBLISH_ATTEMPT;
        PUBLISH_PENDING       = false;
        MQTT_PUBLISH_FAILURES = 0;
        return;
    }

    // * Hold on to the values and try again shortly
    PUBLISH_PENDING = true;
    MQTT_PUBLISH_FAILURES++;

    Serial.printf("MQTT publish failed (%u in a row), keeping data for retry\n", MQTT_PUBLISH_FAILURES);

    // * PubSubClient can keep reporting connected() while the tcp socket is
    // * already dead. Tear the connection down so the reconnect logic runs.
    if (MQTT_PUBLISH_FAILURES >= MQTT_MAX_PUBLISH_FAILURES)
    {
        Serial.println(F("MQTT connection looks stale, forcing a reconnect"));
        mqtt_client.disconnect();
        MQTT_PUBLISH_FAILURES  = 0;
        LAST_RECONNECT_ATTEMPT = millis() - MQTT_RECONNECT_INTERVAL;
    }
}

// **********************************
// * Failsafe                       *
// **********************************

// * Last resort when wifi or the broker stay unreachable for a long time
void handle_failsafe()
{
    unsigned long now = millis();

    if (WiFi.status() == WL_CONNECTED)
        LAST_WIFI_CONNECTED = now;

    if (now - LAST_WIFI_CONNECTED > WIFI_FAILSAFE_TIMEOUT)
    {
        Serial.println(F("*** No wifi connection for too long, restarting ..."));
        Serial.flush();
        ESP.restart();
    }

    if (now - LAST_MQTT_CONNECTED > MQTT_FAILSAFE_TIMEOUT)
    {
        Serial.println(F("*** No mqtt connection for too long, restarting ..."));
        Serial.flush();
        ESP.restart();
    }

    // * The meter is still feeding us telegrams and the client still claims to
    // * be connected, yet nothing has actually reached the broker for a long
    // * time. Something is wedged somewhere below us, so start over.
    if (DATA_READY && LAST_UPDATE_SENT != 0 &&
        now - LAST_TELEGRAM_RECEIVED < P1_TELEGRAM_STALE_TIMEOUT &&
        now - LAST_UPDATE_SENT > MQTT_FAILSAFE_TIMEOUT)
    {
        Serial.println(F("*** Telegrams are coming in but nothing reaches the broker, restarting ..."));
        Serial.flush();
        ESP.restart();
    }
}

// **********************************
// * P1                             *
// **********************************

unsigned int CRC16(unsigned int crc, unsigned char *buf, int len)
{
	for (int pos = 0; pos < len; pos++)
    {
		crc ^= (unsigned int)buf[pos];    // * XOR byte into least sig. byte of crc
                                          // * Loop over each bit
        for (int i = 8; i != 0; i--)
        {
            // * If the LSB is set
            if ((crc & 0x0001) != 0)
            {
                // * Shift right and XOR 0xA001
                crc >>= 1;
				crc ^= 0xA001;
			}
            // * Else LSB is not set
            else
                // * Just shift right
                crc >>= 1;
		}
	}
	return crc;
}

bool isNumber(char *res, int len)
{
    for (int i = 0; i < len; i++)
    {
        if (((res[i] < '0') || (res[i] > '9')) && (res[i] != '.' && res[i] != 0))
            return false;
    }
    return true;
}

int FindCharInArrayRev(char array[], char c, int len)
{
    for (int i = len - 1; i >= 0; i--)
    {
        if (array[i] == c)
            return i;
    }
    return -1;
}

long getValue(char *buffer, int maxlen, char startchar, char endchar)
{
    int s = FindCharInArrayRev(buffer, startchar, maxlen - 2);
    int l = FindCharInArrayRev(buffer, endchar, maxlen - 2) - s - 1;

    char res[16];
    memset(res, 0, sizeof(res));

    // * A malformed line can leave us without a start or end char, which used
    // * to turn into a huge length for strncpy below
    if (s < 0 || l < 0 || l >= (int) sizeof(res))
        return 0;

    if (strncpy(res, buffer + s + 1, l))
    {
        if (endchar == '*')
        {
            if (isNumber(res, l))
                // * Lazy convert float to long
                return (1000 * atof(res));
        }
        else if (endchar == ')')
        {
            if (isNumber(res, l))
                return atof(res);
        }
    }
    return 0;
}

bool decode_telegram(int len)
{
    int startChar = FindCharInArrayRev(telegram, '/', len);
    int endChar = FindCharInArrayRev(telegram, '!', len);
    bool validCRCFound = false;

#if DEBUG_TELEGRAM
    for (int cnt = 0; cnt < len; cnt++) {
        Serial.print(telegram[cnt]);
    }
    Serial.print("\n");
#endif

    if (startChar >= 0)
    {
        // * Start found. Reset CRC calculation
        currentCRC = CRC16(0x0000,(unsigned char *) telegram+startChar, len-startChar);
    }
    else if (endChar >= 0)
    {
        // * Add to crc calc
        currentCRC = CRC16(currentCRC,(unsigned char*)telegram+endChar, 1);

        char messageCRC[5];
        strncpy(messageCRC, telegram + endChar + 1, 4);

        messageCRC[4] = 0;   // * Thanks to HarmOtten (issue 5)
        validCRCFound = (strtol(messageCRC, NULL, 16) == currentCRC);

        if (validCRCFound)
            Serial.println(F("CRC Valid!"));
        else
            Serial.println(F("CRC Invalid!"));

        currentCRC = 0;
    }
    else
    {
        currentCRC = CRC16(currentCRC, (unsigned char*) telegram, len);
    }

    // 1-0:1.8.1(000992.992*kWh)
    // 1-0:1.8.1 = Elektra verbruik laag tarief (DSMR v4.0)
    if (strncmp(telegram, "1-0:1.8.1", strlen("1-0:1.8.1")) == 0)
    {
        CONSUMPTION_LOW_TARIF = getValue(telegram, len, '(', '*');
    }

    // 1-0:1.8.2(000560.157*kWh)
    // 1-0:1.8.2 = Elektra verbruik hoog tarief (DSMR v4.0)
    if (strncmp(telegram, "1-0:1.8.2", strlen("1-0:1.8.2")) == 0)
    {
        CONSUMPTION_HIGH_TARIF = getValue(telegram, len, '(', '*');
    }
	
    // 1-0:2.8.1(000560.157*kWh)
    // 1-0:2.8.1 = Elektra teruglevering laag tarief (DSMR v4.0)
    if (strncmp(telegram, "1-0:2.8.1", strlen("1-0:2.8.1")) == 0)
    {
        RETURNDELIVERY_LOW_TARIF = getValue(telegram, len, '(', '*');
    }

    // 1-0:2.8.2(000560.157*kWh)
    // 1-0:2.8.2 = Elektra teruglevering hoog tarief (DSMR v4.0)
    if (strncmp(telegram, "1-0:2.8.2", strlen("1-0:2.8.2")) == 0)
    {
        RETURNDELIVERY_HIGH_TARIF = getValue(telegram, len, '(', '*');
    }

    // 1-0:1.7.0(00.424*kW) Actueel verbruik
    // 1-0:1.7.x = Electricity consumption actual usage (DSMR v4.0)
    if (strncmp(telegram, "1-0:1.7.0", strlen("1-0:1.7.0")) == 0)
    {
        ACTUAL_CONSUMPTION = getValue(telegram, len, '(', '*');
    }

    // 1-0:2.7.0(00.000*kW) Actuele teruglevering (-P) in 1 Watt resolution
    if (strncmp(telegram, "1-0:2.7.0", strlen("1-0:2.7.0")) == 0)
    {
        ACTUAL_RETURNDELIVERY = getValue(telegram, len, '(', '*');
    }

    // 1-0:21.7.0(00.378*kW)
    // 1-0:21.7.0 = Instantaan vermogen Elektriciteit levering L1
    if (strncmp(telegram, "1-0:21.7.0", strlen("1-0:21.7.0")) == 0)
    {
        L1_INSTANT_POWER_USAGE = getValue(telegram, len, '(', '*');
    }

    // 1-0:41.7.0(00.378*kW)
    // 1-0:41.7.0 = Instantaan vermogen Elektriciteit levering L2
    if (strncmp(telegram, "1-0:41.7.0", strlen("1-0:41.7.0")) == 0)
    {
        L2_INSTANT_POWER_USAGE = getValue(telegram, len, '(', '*');
    }

    // 1-0:61.7.0(00.378*kW)
    // 1-0:61.7.0 = Instantaan vermogen Elektriciteit levering L3
    if (strncmp(telegram, "1-0:61.7.0", strlen("1-0:61.7.0")) == 0)
    {
        L3_INSTANT_POWER_USAGE = getValue(telegram, len, '(', '*');
    }

    // 1-0:31.7.0(002*A)
    // 1-0:31.7.0 = Instantane stroom Elektriciteit L1
    if (strncmp(telegram, "1-0:31.7.0", strlen("1-0:31.7.0")) == 0)
    {
        L1_INSTANT_POWER_CURRENT = getValue(telegram, len, '(', '*');
    }
    // 1-0:51.7.0(002*A)
    // 1-0:51.7.0 = Instantane stroom Elektriciteit L2
    if (strncmp(telegram, "1-0:51.7.0", strlen("1-0:51.7.0")) == 0)
    {
        L2_INSTANT_POWER_CURRENT = getValue(telegram, len, '(', '*');
    }
    // 1-0:71.7.0(002*A)
    // 1-0:71.7.0 = Instantane stroom Elektriciteit L3
    if (strncmp(telegram, "1-0:71.7.0", strlen("1-0:71.7.0")) == 0)
    {
        L3_INSTANT_POWER_CURRENT = getValue(telegram, len, '(', '*');
    }

    // 1-0:32.7.0(232.0*V)
    // 1-0:32.7.0 = Voltage L1
    if (strncmp(telegram, "1-0:32.7.0", strlen("1-0:32.7.0")) == 0)
    {
        L1_VOLTAGE = getValue(telegram, len, '(', '*');
    }
    // 1-0:52.7.0(232.0*V)
    // 1-0:52.7.0 = Voltage L2
    if (strncmp(telegram, "1-0:52.7.0", strlen("1-0:52.7.0")) == 0)
    {
        L2_VOLTAGE = getValue(telegram, len, '(', '*');
    }   
    // 1-0:72.7.0(232.0*V)
    // 1-0:72.7.0 = Voltage L3
    if (strncmp(telegram, "1-0:72.7.0", strlen("1-0:72.7.0")) == 0)
    {
        L3_VOLTAGE = getValue(telegram, len, '(', '*');
    }

    // 0-1:24.2.1(150531200000S)(00811.923*m3)
    // 0-1:24.2.1 = Gas (DSMR v4.0) on Kaifa MA105 meter
    if (strncmp(telegram, "0-1:24.2.1", strlen("0-1:24.2.1")) == 0)
    {
        GAS_METER_M3 = getValue(telegram, len, '(', '*');
    }

    // 0-0:96.14.0(0001)
    // 0-0:96.14.0 = Actual Tarif
    if (strncmp(telegram, "0-0:96.14.0", strlen("0-0:96.14.0")) == 0)
    {
        ACTUAL_TARIF = getValue(telegram, len, '(', ')');
    }

    // 0-0:96.7.21(00003)
    // 0-0:96.7.21 = Aantal onderbrekingen Elektriciteit
    if (strncmp(telegram, "0-0:96.7.21", strlen("0-0:96.7.21")) == 0)
    {
        SHORT_POWER_OUTAGES = getValue(telegram, len, '(', ')');
    }

    // 0-0:96.7.9(00001)
    // 0-0:96.7.9 = Aantal lange onderbrekingen Elektriciteit
    if (strncmp(telegram, "0-0:96.7.9", strlen("0-0:96.7.9")) == 0)
    {
        LONG_POWER_OUTAGES = getValue(telegram, len, '(', ')');
    }

    // 1-0:32.32.0(00000)
    // 1-0:32.32.0 = Aantal korte spanningsdalingen Elektriciteit in fase 1
    if (strncmp(telegram, "1-0:32.32.0", strlen("1-0:32.32.0")) == 0)
    {
        SHORT_POWER_DROPS = getValue(telegram, len, '(', ')');
    }

    // 1-0:32.36.0(00000)
    // 1-0:32.36.0 = Aantal korte spanningsstijgingen Elektriciteit in fase 1
    if (strncmp(telegram, "1-0:32.36.0", strlen("1-0:32.36.0")) == 0)
    {
        SHORT_POWER_PEAKS = getValue(telegram, len, '(', ')');
    }

    return validCRCFound;
}

// * Drain whatever the meter has sent us. This never blocks: it only reads the
// * bytes that are already in the uart buffer and hands over complete lines.
void read_p1_hardwareserial()
{
    static int telegram_pos = 0;
    static bool line_overflow = false;

    while (Serial.available())
    {
        char incoming = (char) Serial.read();

        if (incoming == '\n')
        {
            // * A line that did not fit is dropped, we resync on this newline
            if (!line_overflow)
                processLine(telegram_pos);

            telegram_pos   = 0;
            line_overflow  = false;
            continue;
        }

        // * Leave room for the '\n' and the terminating 0 that processLine adds,
        // * the crc is calculated over the line including its line ending
        if (telegram_pos < P1_MAXLINELENGTH - 2)
        {
            telegram[telegram_pos] = incoming;
            telegram_pos++;
        }
        else
        {
            line_overflow = true;
        }
    }
}

void processLine(int len) {
    telegram[len] = '\n';
    telegram[len + 1] = 0;
    yield();

    if (decode_telegram(len + 1))
    {
        // * A complete telegram passed the crc check. Cache the decoded values,
        // * loop() decides when they go out to the broker.
        LAST_TELEGRAM_RECEIVED = millis();

        // * Get the very first reading out to the broker right away instead of
        // * leaving Home Assistant without a value for a whole update interval
        if (!DATA_READY)
            PUBLISH_PENDING = true;

        DATA_READY = true;
    }
}

// **********************************
// * Web interface                  *
// **********************************

String html_escape(const char *value)
{
    String out;

    for (const char *p = value; *p; p++)
    {
        switch (*p)
        {
            case '&':  out += F("&amp;");  break;
            case '<':  out += F("&lt;");   break;
            case '>':  out += F("&gt;");   break;
            case '"':  out += F("&quot;"); break;
            case '\'': out += F("&#39;");  break;
            default:   out += *p;          break;
        }
    }
    return out;
}

String format_duration(unsigned long ms)
{
    unsigned long seconds = ms / 1000;
    unsigned long days    = seconds / 86400;
    unsigned long hours   = (seconds % 86400) / 3600;
    unsigned long minutes = (seconds % 3600) / 60;

    seconds = seconds % 60;

    String out;

    if (days > 0)
    {
        out += days;
        out += F("d ");
    }
    if (days > 0 || hours > 0)
    {
        out += hours;
        out += F("h ");
    }
    if (days > 0 || hours > 0 || minutes > 0)
    {
        out += minutes;
        out += F("m ");
    }

    out += seconds;
    out += F("s");

    return out;
}

// * "12s ago" or "never" for the timestamps on the status page
String time_since(unsigned long timestamp)
{
    if (timestamp == 0)
        return F("never");

    String out = format_duration(millis() - timestamp);
    out += F(" ago");
    return out;
}

String table_row(const __FlashStringHelper *name, const String &value)
{
    String out = F("<tr><td>");
    out += name;
    out += F("</td><td>");
    out += value;
    out += F("</td></tr>");
    return out;
}

// * Accept only what can actually be a hostname or an ip address. Keeps
// * nonsense out of the settings and out of the json on /status.json
bool is_valid_host(const String &host)
{
    for (unsigned int i = 0; i < host.length(); i++)
    {
        char c = host[i];

        if (!isalnum(c) && c != '.' && c != '-' && c != '_' && c != ':')
            return false;
    }
    return true;
}

// * Every page sits behind basic auth, there is no open access
bool web_authenticated()
{
    if (server.authenticate(WEB_USER, WEB_PASS))
        return true;

    server.requestAuthentication();
    return false;
}

void handle_root()
{
    if (!web_authenticated())
        return;

    bool connected = mqtt_client.connected();

    String page;
    page.reserve(4096);

    page += FPSTR(PAGE_HEADER);

    page += F("<div class=\"card\"><h2>MQTT</h2><table>");
    page += table_row(F("Status"), connected
        ? String(F("<span class=\"ok\">connected</span>"))
        : String(F("<span class=\"bad\">disconnected</span>")));
    page += table_row(F("Broker"), html_escape(MQTT_HOST) + ":" + html_escape(MQTT_PORT));
    page += table_row(F("Client id"), html_escape(MQTT_CLIENT_ID));
    page += table_row(F("Last client state"), String(MQTT_LAST_STATE));
    page += table_row(F("Failed connects in a row"), String(MQTT_RECONNECT_ATTEMPTS));
    page += table_row(F("Last succesful send"), time_since(LAST_UPDATE_SENT));
    page += table_row(F("Data waiting to be sent"), PUBLISH_PENDING ? F("yes") : F("no"));
    page += F("</table>");
    page += F("<form method=\"post\" action=\"/reconnect\" class=\"inline\">"
              "<button class=\"secondary\">Reconnect MQTT</button></form> "
              "<form method=\"post\" action=\"/reboot\" class=\"inline\">"
              "<button class=\"secondary\">Reboot device</button></form>");
    page += F("</div>");

    page += F("<div class=\"card\"><h2>P1 meter</h2><table>");
    page += table_row(F("Last valid telegram"), time_since(LAST_TELEGRAM_RECEIVED));
    page += table_row(F("Actual consumption (W)"), String(ACTUAL_CONSUMPTION));
    page += table_row(F("Actual return delivery (W)"), String(ACTUAL_RETURNDELIVERY));
    page += table_row(F("Consumption low tarif (Wh)"), String(CONSUMPTION_LOW_TARIF));
    page += table_row(F("Consumption high tarif (Wh)"), String(CONSUMPTION_HIGH_TARIF));
    page += table_row(F("Gas meter (dm3)"), String(GAS_METER_M3));
    page += F("</table></div>");

    page += F("<div class=\"card\"><h2>Device</h2><table>");
    page += table_row(F("Hostname"), F(HOSTNAME ".local"));
    page += table_row(F("IP address"), WiFi.localIP().toString());
    page += table_row(F("Wifi network"), html_escape(WiFi.SSID().c_str()));
    page += table_row(F("Wifi signal"), String(WiFi.RSSI()) + F(" dBm"));
    page += table_row(F("Uptime"), format_duration(millis()));
    page += table_row(F("Free heap"), String(ESP.getFreeHeap()) + F(" bytes"));
    page += F("</table></div>");

    if (strcmp(WEB_PASS, WEB_PASSWORD) == 0)
        page += F("<div class=\"card warn\">This page still uses the default password. "
                  "Change it under <a href=\"/config\">Settings</a>.</div>");

    page += FPSTR(PAGE_FOOTER);

    server.send(200, F("text/html"), page);
}

// * Render the settings form, optionally with a message on top
void send_config_page(const __FlashStringHelper *message, bool is_error)
{
    String page;
    page.reserve(4096);

    page += FPSTR(PAGE_HEADER);

    if (message)
    {
        page += F("<div class=\"card\"><span class=\"");
        page += is_error ? F("bad") : F("ok");
        page += F("\">");
        page += message;
        page += F("</span></div>");
    }

    page += F("<form method=\"post\" action=\"/config\">");

    page += F("<div class=\"card\"><h2>MQTT broker</h2>");
    page += F("<label for=\"mqtt_host\">Hostname or IP address</label>"
              "<input id=\"mqtt_host\" name=\"mqtt_host\" maxlength=\"63\" required value=\"");
    page += html_escape(MQTT_HOST);
    page += F("\">");
    page += F("<label for=\"mqtt_port\">Port</label>"
              "<input id=\"mqtt_port\" name=\"mqtt_port\" maxlength=\"5\" required value=\"");
    page += html_escape(MQTT_PORT);
    page += F("\">");
    page += F("<label for=\"mqtt_user\">Username</label>"
              "<input id=\"mqtt_user\" name=\"mqtt_user\" maxlength=\"31\" value=\"");
    page += html_escape(MQTT_USER);
    page += F("\">");
    page += F("<label for=\"mqtt_pass\">Password</label>"
              "<input id=\"mqtt_pass\" name=\"mqtt_pass\" type=\"password\" maxlength=\"31\" "
              "autocomplete=\"new-password\" placeholder=\"unchanged\">");
    page += F("<p class=\"note\">Leave the password empty to keep the current one.</p></div>");

    page += F("<div class=\"card\"><h2>This web interface</h2>");
    page += F("<label for=\"web_user\">Username</label>"
              "<input id=\"web_user\" name=\"web_user\" maxlength=\"31\" required value=\"");
    page += html_escape(WEB_USER);
    page += F("\">");
    page += F("<label for=\"web_pass\">Password</label>"
              "<input id=\"web_pass\" name=\"web_pass\" type=\"password\" maxlength=\"31\" "
              "autocomplete=\"new-password\" placeholder=\"unchanged\">");
    page += F("<p class=\"note\">Leave the password empty to keep the current one. "
              "After changing these your browser will ask you to log in again.</p></div>");

    page += F("<button type=\"submit\">Save settings</button></form>");

    page += FPSTR(PAGE_FOOTER);

    server.send(200, F("text/html"), page);
}

void handle_config()
{
    if (!web_authenticated())
        return;

    send_config_page(NULL, false);
}

void handle_config_save()
{
    if (!web_authenticated())
        return;

    String mqtt_host = server.arg("mqtt_host");
    String mqtt_port = server.arg("mqtt_port");
    String mqtt_user = server.arg("mqtt_user");
    String mqtt_pass = server.arg("mqtt_pass");
    String web_user  = server.arg("web_user");
    String web_pass  = server.arg("web_pass");

    mqtt_host.trim();
    mqtt_port.trim();
    web_user.trim();

    if (mqtt_host.length() == 0 || mqtt_host.length() >= sizeof(MQTT_HOST) ||
        !is_valid_host(mqtt_host))
    {
        send_config_page(F("Please fill in a valid mqtt hostname or ip address."), true);
        return;
    }

    long port = mqtt_port.toInt();

    if (port < 1 || port > 65535)
    {
        send_config_page(F("The mqtt port must be a number between 1 and 65535."), true);
        return;
    }

    if (mqtt_user.length() >= sizeof(MQTT_USER) || mqtt_pass.length() >= sizeof(MQTT_PASS))
    {
        send_config_page(F("The mqtt username or password is too long."), true);
        return;
    }

    if (web_user.length() == 0 || web_user.length() >= sizeof(WEB_USER) ||
        web_pass.length() >= sizeof(WEB_PASS))
    {
        send_config_page(F("Please fill in a valid username and password for this page."), true);
        return;
    }

    strlcpy(MQTT_HOST, mqtt_host.c_str(), sizeof(MQTT_HOST));
    snprintf(MQTT_PORT, sizeof(MQTT_PORT), "%ld", port);
    strlcpy(MQTT_USER, mqtt_user.c_str(), sizeof(MQTT_USER));
    strlcpy(WEB_USER,  web_user.c_str(),  sizeof(WEB_USER));

    // * An empty password field means "keep what we have"
    if (mqtt_pass.length() > 0)
        strlcpy(MQTT_PASS, mqtt_pass.c_str(), sizeof(MQTT_PASS));

    if (web_pass.length() > 0)
        strlcpy(WEB_PASS, web_pass.c_str(), sizeof(WEB_PASS));

    if (!save_config())
    {
        send_config_page(F("Could not write the settings to EEPROM."), true);
        return;
    }

    // * Takes effect right away, no reflash and no reboot needed
    apply_mqtt_settings();

    send_config_page(F("Settings saved, reconnecting to the broker."), false);
}

void handle_reconnect()
{
    if (!web_authenticated())
        return;

    Serial.println(F("Manual mqtt reconnect requested from the web interface"));
    apply_mqtt_settings();

    server.sendHeader(F("Location"), F("/"));
    server.send(303, F("text/plain"), F("Reconnecting"));
}

void handle_reboot()
{
    if (!web_authenticated())
        return;

    Serial.println(F("Reboot requested from the web interface"));

    String page;
    page += FPSTR(PAGE_HEADER);
    page += F("<div class=\"card\">Rebooting, this page will come back in a few seconds. "
              "<a href=\"/\">Back to status</a></div>");
    page += FPSTR(PAGE_FOOTER);

    server.send(200, F("text/html"), page);

    // * Reboot from loop() so the response is actually flushed to the browser.
    // * 0 means "nothing scheduled", so step over it on a millis() rollover.
    REBOOT_AT = millis() + 500;

    if (REBOOT_AT == 0)
        REBOOT_AT = 1;
}

// * Small machine readable version of the status page
void handle_status_json()
{
    if (!web_authenticated())
        return;

    String json;
    json.reserve(512);

    json += F("{\"mqtt_connected\":");
    json += mqtt_client.connected() ? F("true") : F("false");
    json += F(",\"mqtt_state\":");
    json += MQTT_LAST_STATE;
    json += F(",\"mqtt_host\":\"");
    json += MQTT_HOST;
    json += F("\",\"mqtt_port\":");
    json += MQTT_PORT;
    json += F(",\"client_id\":\"");
    json += MQTT_CLIENT_ID;
    json += F("\",\"publish_pending\":");
    json += PUBLISH_PENDING ? F("true") : F("false");
    json += F(",\"seconds_since_last_send\":");
    json += LAST_UPDATE_SENT == 0 ? -1 : (long) ((millis() - LAST_UPDATE_SENT) / 1000);
    json += F(",\"seconds_since_last_telegram\":");
    json += LAST_TELEGRAM_RECEIVED == 0 ? -1 : (long) ((millis() - LAST_TELEGRAM_RECEIVED) / 1000);
    json += F(",\"rssi\":");
    json += WiFi.RSSI();
    json += F(",\"uptime_seconds\":");
    json += (long) (millis() / 1000);
    json += F(",\"free_heap\":");
    json += ESP.getFreeHeap();
    json += F("}");

    server.send(200, F("application/json"), json);
}

void handle_not_found()
{
    server.send(404, F("text/plain"), F("Not found"));
}

void setup_web()
{
    server.on("/",            HTTP_GET,  handle_root);
    server.on("/config",      HTTP_GET,  handle_config);
    server.on("/config",      HTTP_POST, handle_config_save);
    server.on("/reconnect",   HTTP_POST, handle_reconnect);
    server.on("/reboot",      HTTP_POST, handle_reboot);
    server.on("/status.json", HTTP_GET,  handle_status_json);
    server.onNotFound(handle_not_found);

    server.begin();

    Serial.printf("Web interface started on http://%s:%d/ (and http://%s.local:%d/)\n",
                  WiFi.localIP().toString().c_str(), WEB_PORT, HOSTNAME, WEB_PORT);
}

// ******************************************
// * Callback for saving WIFI config        *
// ******************************************

bool shouldSaveConfig = false;

// * Callback notifying us of the need to save config
void save_wifi_config_callback ()
{
    Serial.println(F("Should save config"));
    shouldSaveConfig = true;
}

// **********************************
// * Setup OTA                      *
// **********************************

void setup_ota()
{
    Serial.println(F("Arduino OTA activated."));

    // * Port defaults to 8266
    ArduinoOTA.setPort(8266);

    // * Set hostname for OTA
    ArduinoOTA.setHostname(HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);

    ArduinoOTA.onStart([]()
    {
        Serial.println(F("Arduino OTA: Start"));
    });

    ArduinoOTA.onEnd([]()
    {
        Serial.println(F("Arduino OTA: End (Running reboot)"));
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
    {
        Serial.printf("Arduino OTA Progress: %u%%\r", (progress / (total / 100)));
    });

    ArduinoOTA.onError([](ota_error_t error)
    {
        Serial.printf("Arduino OTA Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR)
            Serial.println(F("Arduino OTA: Auth Failed"));
        else if (error == OTA_BEGIN_ERROR)
            Serial.println(F("Arduino OTA: Begin Failed"));
        else if (error == OTA_CONNECT_ERROR)
            Serial.println(F("Arduino OTA: Connect Failed"));
        else if (error == OTA_RECEIVE_ERROR)
            Serial.println(F("Arduino OTA: Receive Failed"));
        else if (error == OTA_END_ERROR)
            Serial.println(F("Arduino OTA: End Failed"));
    });

    ArduinoOTA.begin();
    Serial.println(F("Arduino OTA finished"));
}

// **********************************
// * Setup MDNS discovery service   *
// **********************************

void setup_mdns()
{
    Serial.println(F("Starting MDNS responder service"));

    bool mdns_result = MDNS.begin(HOSTNAME);
    if (mdns_result)
    {
        MDNS.addService("http", "tcp", WEB_PORT);
    }
}

// **********************************
// * Setup Main                     *
// **********************************

void setup()
{
    // * Configure EEPROM
    EEPROM.begin(512);

    // A bigger receive buffer means a slow loop iteration can no longer eat
    // into the telegram that is being received at that moment
    Serial.setRxBufferSize(SERIAL_RX_BUFFER_SIZE);

    // Setup a hw serial connection for communication with the P1 meter and logging (not using inversion)
    Serial.begin(BAUD_RATE, SERIAL_8N1, SERIAL_FULL);
    Serial.println("");
    Serial.println("Swapping UART0 RX to inverted");
    Serial.flush();

    // Invert the RX serialport by setting a register value, this way the TX might continue normally allowing the serial monitor to read println's
    USC0(UART0) = USC0(UART0) | BIT(UCRXI);
    Serial.println("Serial port is ready to recieve.");

    // * Set led pin as output
    pinMode(LED_BUILTIN, OUTPUT);

    // * Start ticker with 0.5 because we start in AP mode and try to connect
    ticker.attach(0.6, tick);

    // * Load the stored MQTT and web settings, falling back to settings.h
    load_config();

    WiFiManagerParameter CUSTOM_MQTT_HOST("host", "MQTT hostname", MQTT_HOST, 64);
    WiFiManagerParameter CUSTOM_MQTT_PORT("port", "MQTT port",     MQTT_PORT, 6);
    WiFiManagerParameter CUSTOM_MQTT_USER("user", "MQTT user",     MQTT_USER, 32);
    WiFiManagerParameter CUSTOM_MQTT_PASS("pass", "MQTT pass",     MQTT_PASS, 32);

    // * WiFiManager local initialization. Once its business is done, there is no need to keep it around
    WiFiManager wifiManager;

    // * Reset settings - uncomment for testing
    // wifiManager.resetSettings();

    // * Set callback that gets called when connecting to previous WiFi fails, and enters Access Point mode
    wifiManager.setAPCallback(configModeCallback);

    // * Set timeout
    wifiManager.setConfigPortalTimeout(WIFI_TIMEOUT);

    // * Set save config callback
    wifiManager.setSaveConfigCallback(save_wifi_config_callback);

    // * Add all your parameters here
    wifiManager.addParameter(&CUSTOM_MQTT_HOST);
    wifiManager.addParameter(&CUSTOM_MQTT_PORT);
    wifiManager.addParameter(&CUSTOM_MQTT_USER);
    wifiManager.addParameter(&CUSTOM_MQTT_PASS);

    // * Fetches SSID and pass and tries to connect
    // * Reset when no connection after 10 seconds
    if (!wifiManager.autoConnect())
    {
        Serial.println(F("Failed to connect to WIFI and hit timeout"));

        // * Reset and try again, or maybe put it to deep sleep
        ESP.reset();
        delay(WIFI_TIMEOUT);
    }

    // * Read updated parameters
    strlcpy(MQTT_HOST, CUSTOM_MQTT_HOST.getValue(), sizeof(MQTT_HOST));
    strlcpy(MQTT_PORT, CUSTOM_MQTT_PORT.getValue(), sizeof(MQTT_PORT));
    strlcpy(MQTT_USER, CUSTOM_MQTT_USER.getValue(), sizeof(MQTT_USER));
    strlcpy(MQTT_PASS, CUSTOM_MQTT_PASS.getValue(), sizeof(MQTT_PASS));

    // * Save the custom parameters to EEPROM
    if (shouldSaveConfig)
    {
        Serial.println(F("Saving WiFiManager config"));
        save_config();
    }

    // * If you get here you have connected to the WiFi
    Serial.println(F("Connected to WIFI..."));

    // * Reconnect on our own whenever the access point disappears for a while
    WiFi.setAutoReconnect(true);

    // * Keep LED on
    ticker.detach();
    digitalWrite(LED_BUILTIN, LOW);

    // * Configure OTA
    setup_ota();

    // * Startup MDNS Service
    setup_mdns();

    // * Startup the web configuration interface
    setup_web();

    // * A unique client id keeps a hung session on the broker from kicking the
    // * fresh connection straight off again
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(MQTT_CLIENT_ID, sizeof(MQTT_CLIENT_ID), "%s-%02x%02x%02x", HOSTNAME, mac[3], mac[4], mac[5]);

    // * Never let a dead broker or a bad route stall the main loop for long
    espClient.setTimeout(3000);
    mqtt_client.setSocketTimeout(3);

    // * Setup MQTT
    Serial.printf("MQTT connecting to: %s:%s\n", MQTT_HOST, MQTT_PORT);
    mqtt_client.setServer(MQTT_HOST, atoi(MQTT_PORT));

    // * Start the failsafe timers now, so a slow first connect does not
    // * immediately trigger a reboot
    unsigned long now = millis();
    LAST_MQTT_CONNECTED    = now;
    LAST_WIFI_CONNECTED    = now;
    LAST_RECONNECT_ATTEMPT = now - MQTT_RECONNECT_INTERVAL;
}

// **********************************
// * Loop                           *
// **********************************

void loop()
{
    // * Reading the meter comes first and happens on every single iteration:
    // * the telegrams keep coming in whether we have a broker to send them to
    // * or not, and nothing below this line is allowed to stall for long
    read_p1_hardwareserial();

    ArduinoOTA.handle();
    MDNS.update();

    // * A slow http client can hold this up for a few seconds (the web server
    // * has its own fixed timeouts). The enlarged uart buffer absorbs that, and
    // * worst case we miss a telegram or two, which the next second replaces.
    server.handleClient();

    handle_mqtt();
    handle_publish();
    handle_failsafe();

    // * Deferred reboot requested by the web interface
    if (REBOOT_AT != 0 && (long) (millis() - REBOOT_AT) >= 0)
    {
        Serial.flush();
        ESP.restart();
    }
}
