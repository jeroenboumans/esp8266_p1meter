# esp8266_p1meter

Software for the ESP2866 that sends P1 smart meter data to an mqtt broker (with OTA firmware updates)

## about this fork
This fork (tries) to add support for the `Landys and Gyr E360` smartmeter (DSMR5.0)

The ![original source](https://github.com/fliphess/esp8266_p1meter) has issues with DSMR5.0 meters who like to send telegrams every 1 second at a high 115200 baud rate. 
This causes the used SoftwareSerial to struggle to keep up and thus only receives corrupted messages. This fork switches to using the main Hardware serial port (RX) for communication with the meter.

# Getting started
This setup requires:
- An esp8266 (nodeMcu and Wemos d1 mini have been tested)
- A 10k ohm resistor
- A 4 pin RJ11 or [6 pin RJ12 cable](https://www.tinytronics.nl/shop/nl/kabels/adapters/rj12-naar-6-pins-dupont-jumper-adapter) Both cables work great, but a 6 pin cable can also power the esp8266 on most DSMR5+ meters.

Compiling up using Arduino IDE:
- Ensure you have selected the right board
- Using the Tools->Manage Libraries... install `PubSubClient` and `WifiManager`
- In the file `Settings.h` change `OTA_PASSWORD` and `WEB_PASSWORD` to safe secret values
- Flash the software

Compiling up using PlatformIO:
- Ensure the correct board type is selected in project configuration
- In the file `Settings.h` change `OTA_PASSWORD` and `WEB_PASSWORD` to safe secret values
- Upload the software.

Finishing off:
- You should now see a new wifi network `ESP******` connect to this wifi network, a popup should appear, else manually navigate to `192.168.4.1`
- Configure your wifi and Mqtt settings
- To check if everything is up and running you can listen to the MQTT topic `hass/status`, on startup a single message is sent.
- Everything else can be changed afterwards from the [web interface](#web-interface) at `http://p1meter.local/`, no reflashing needed.

## Connecting to the P1 meter
Connect the esp8266 to an RJ11 cable/connector following the diagram.

| P1 pin   | ESP8266 Pin |
| ----     | ---- |
| 2 - RTS  | 3.3v |
| 3 - GND  | GND  |
| 4 -      |      |
| 5 - RXD (data) | RX (gpio3) |

On most Landys and Gyr models a 10K resistor should be used between the ESP's 3.3v and the p1's DATA (RXD) pin. Many howto's mention RTS requires 5V (VIN) to activate the P1 port, but for me 3V3 suffices.

![Wiring](https://raw.githubusercontent.com/daniel-jong/esp8266_p1meter/master/assets/esp8266_p1meter_bb.png)

### Optional: Powering the esp8266 using your DSMR5+ meter 
<details><summary>Expand to see wiring description</summary>
<p>
  
When using a 6 pin cable you can use the power source provided by the meter.
  
| P1 pin   | ESP8266 Pin |
| ----     | ---- |
| 1 - 5v out | 5v or Vin |
| 2 - RTS  | 3.3v |
| 3 - GND  | GND  |
| 4 -      |      |
| 5 - RXD (data) | RX (gpio3) |
| 6 - GND  | GND  |

![Wiring powered by meter](https://raw.githubusercontent.com/daniel-jong/esp8266_p1meter/master/assets/esp8266_p1meter_bb_PoweredByMeter.png)

</p>
</details>

## Web interface

Once the device is on your network it serves a small configuration and status
page on `http://p1meter.local/` (or on its IP address). It is protected with
HTTP basic authentication, using the credentials from `WEB_USERNAME` /
`WEB_PASSWORD` in `settings.h` until you change them on the settings page.

The **status** page shows:

- the MQTT connection state, the broker it is talking to and the client id
- how long ago the last set of values was succesfully sent to the broker
- how long ago the last telegram passed the CRC check
- whether there is data waiting to be sent
- wifi network, signal strength, IP address, uptime and free heap
- buttons to force an MQTT reconnect or to reboot the device

The **settings** page lets you change the MQTT host, port, username and
password, plus the credentials of the web interface itself. Settings are stored
in EEPROM and survive a reboot. Saving them takes effect immediately: the
current MQTT connection is dropped and a new one is opened against the new
broker, without reflashing or rebooting.

Leave a password field empty to keep the password that is currently stored.

`http://p1meter.local/status.json` returns the same status as JSON, which is
handy for debugging or for a REST sensor in Home Assistant.

The WiFiManager captive portal remains the first-time setup path: on a device
that does not know your wifi yet it opens the `ESP******` access point where
you fill in both the wifi and the MQTT settings.

### Security

The web interface trusts your local network: it relies on HTTP basic
authentication only, with no CSRF token and no rate limiting on login
attempts. That is a deliberate tradeoff for a small device that only needs to
be reachable on a trusted home LAN, not the public internet — adding TLS,
sessions or extra dependencies was considered out of scope. Change the
default web and OTA passwords in `settings.h` (or on the settings page) before
exposing the device to any network you don't fully trust.

## Availability

Next to the metrics below, the firmware maintains an availability topic:

```
sensors/power/p1meter/status   online | offline
```

`online` is published (retained) after every succesful connect, and `offline`
is registered with the broker as a Last Will, so it is published on your behalf
the moment the device drops off the network. Point Home Assistant at it to have
the sensors show up as *unavailable* instead of silently keeping stale values:

```yaml
availability_topic: "sensors/power/p1meter/status"
payload_available: "online"
payload_not_available: "offline"
```

## Recovering from a broker outage

The firmware is built to survive a broker (or Home Assistant host) restart on
its own:

- reading and parsing telegrams never waits on MQTT, so nothing is lost while
  the broker is away
- reconnecting is non blocking and backs off from 2 up to 60 seconds between
  attempts
- the last decoded values are kept and retried instead of being dropped when a
  publish fails
- a connection that reports itself as alive but no longer delivers is torn down
  after a few failed publishes
- the client id contains the last three bytes of the MAC address, so a stale
  session on the broker cannot kick the fresh connection off again
- as a last resort the device reboots itself when wifi or the broker have been
  unreachable for five minutes, or when telegrams keep arriving while nothing
  reaches the broker for five minutes

Timings live in `settings.h` (`MQTT_RECONNECT_MIN_INTERVAL`,
`MQTT_RECONNECT_MAX_INTERVAL`, `MQTT_FAILSAFE_TIMEOUT`, `WIFI_FAILSAFE_TIMEOUT`).

## Data Sent

All metrics are send to their own MQTT topic.
The software sends out to the following MQTT topics:

```
sensors/power/p1meter/consumption_low_tarif 2209397
sensors/power/p1meter/consumption_high_tarif 1964962
sensors/power/p1meter/returndelivery_low_tarif 2209397
sensors/power/p1meter/returndelivery_high_tarif 1964962
sensors/power/p1meter/actual_consumption 313
sensors/power/p1meter/actual_returndelivery 0
sensors/power/p1meter/l1_instant_power_usage 313
sensors/power/p1meter/l2_instant_power_usage 0
sensors/power/p1meter/l3_instant_power_usage 0
sensors/power/p1meter/l1_instant_power_current 1000
sensors/power/p1meter/l2_instant_power_current 0
sensors/power/p1meter/l3_instant_power_current 0
sensors/power/p1meter/l1_voltage 233
sensors/power/p1meter/l2_voltage 0
sensors/power/p1meter/l3_voltage 0
sensors/power/p1meter/gas_meter_m3 968922
sensors/power/p1meter/actual_tarif_group 2
sensors/power/p1meter/short_power_outages 3
sensors/power/p1meter/long_power_outages 1
sensors/power/p1meter/short_power_drops 0
sensors/power/p1meter/short_power_peaks 0
sensors/power/p1meter/status online
```

## Home Assistant Configuration

Use this [example](https://raw.githubusercontent.com/daniel-jong/esp8266_p1meter/master/assets/p1_sensors.yaml) for home assistant's `sensor.yaml`

The automatons are yours to create.
And always remember that sending alerts in case of a power outtage only make sense when you own a UPS battery :)

## Thanks to

This sketch is mostly copied and pasted from several other projects.
Standing on the heads of giants, big thanks and great respect to the writers and/or creators of:

- https://github.com/jantenhove/P1-Meter-ESP8266
- https://github.com/neographikal/P1-Meter-ESP8266-MQTT
- http://gejanssen.com/howto/Slimme-meter-uitlezen/
- https://github.com/rroethof/p1reader/
- http://romix.macuser.nl/software.html
- http://blog.regout.info/category/slimmeter/
- http://domoticx.com/p1-poort-slimme-meter-hardware/
