/*

RELAY MODULE

Copyright (C) 2016-2017 by Xose Pérez <xose dot perez at gmail dot com>

*/

#include <EEPROM.h>
#include <Ticker.h>
#include <ArduinoJson.h>
#include <vector>
#include <functional>

typedef struct {
    unsigned char pin;
    bool reverse;
    unsigned char led;
    unsigned long delay_on;
    unsigned long delay_off;
    unsigned int floodWindowStart;
    unsigned char floodWindowChanges;
    bool scheduled;
    unsigned int scheduledStatusTime;
    bool scheduledStatus;
    bool scheduledReport;
    Ticker pulseTicker;
} relay_t;
std::vector<relay_t> _relays;
bool recursive = false;

#if RELAY_PROVIDER == RELAY_PROVIDER_DUAL
unsigned char _dual_status = 0;
#endif

// -----------------------------------------------------------------------------
// RELAY PROVIDERS
// -----------------------------------------------------------------------------

void relayProviderStatus(unsigned char id, bool status) {

    if (id >= _relays.size()) return;

    #if RELAY_PROVIDER == RELAY_PROVIDER_RFBRIDGE
        rfbStatus(id, status);
    #endif

    #if RELAY_PROVIDER == RELAY_PROVIDER_DUAL
        _dual_status ^= (1 << id);
        Serial.flush();
        Serial.write(0xA0);
        Serial.write(0x04);
        Serial.write(_dual_status);
        Serial.write(0xA1);
        Serial.flush();
    #endif

    #if RELAY_PROVIDER == RELAY_PROVIDER_LIGHT
        lightState(status);
    #endif

    #if RELAY_PROVIDER == RELAY_PROVIDER_RELAY
        digitalWrite(_relays[id].pin, _relays[id].reverse ? !status : status);
    #endif

}

bool relayProviderStatus(unsigned char id) {

    if (id >= _relays.size()) return false;

    #if RELAY_PROVIDER == RELAY_PROVIDER_RFBRIDGE
        return _relays[id].scheduledStatus;
    #endif

    #if RELAY_PROVIDER == RELAY_PROVIDER_DUAL
        return ((_dual_status & (1 << id)) > 0);
    #endif

    #if RELAY_PROVIDER == RELAY_PROVIDER_LIGHT
        return lightState();
    #endif

    #if RELAY_PROVIDER == RELAY_PROVIDER_RELAY
        bool status = (digitalRead(_relays[id].pin) == HIGH);
        return _relays[id].reverse ? !status : status;
    #endif

}

// -----------------------------------------------------------------------------
// RELAY
// -----------------------------------------------------------------------------

void relayPulse(unsigned char id) {

    byte relayPulseMode = getSetting("relayPulseMode", RELAY_PULSE_MODE).toInt();
    if (relayPulseMode == RELAY_PULSE_NONE) return;
    long relayPulseTime = 1000.0 * getSetting("relayPulseTime", RELAY_PULSE_TIME).toFloat();
    if (relayPulseTime == 0) return;

    bool status = relayStatus(id);
    bool pulseStatus = (relayPulseMode == RELAY_PULSE_ON);
    if (pulseStatus == status) {
        _relays[id].pulseTicker.detach();
        return;
    }

   _relays[id].pulseTicker.once_ms(relayPulseTime, relayToggle, id);

}

unsigned int relayPulseMode() {
    unsigned int value = getSetting("relayPulseMode", RELAY_PULSE_MODE).toInt();
    return value;
}

void relayPulseMode(unsigned int value, bool report) {

    setSetting("relayPulseMode", value);

    /*
    if (report) {
        char topic[strlen(MQTT_TOPIC_RELAY) + 10];
        sprintf(topic, "%s/pulse", MQTT_TOPIC_RELAY);
        char value[2];
        sprintf(value, "%d", value);
        mqttSend(topic, value);
    }
    */

    char message[20];
    sprintf(message, "{\"relayPulseMode\": %d}", value);
    wsSend(message);

}

void relayPulseMode(unsigned int value) {
    relayPulseMode(value, true);
}

void relayPulseToggle() {
    unsigned int value = relayPulseMode();
    value = (value == RELAY_PULSE_NONE) ? RELAY_PULSE_OFF : RELAY_PULSE_NONE;
    relayPulseMode(value);
}

bool relayStatus(unsigned char id, bool status, bool report) {

    if (id >= _relays.size()) return false;

    bool changed = false;

    #if TRACK_RELAY_STATUS
    if (relayStatus(id) != status) {
    #endif

        unsigned int currentTime = millis();
        unsigned int floodWindowEnd = _relays[id].floodWindowStart + 1000 * RELAY_FLOOD_WINDOW;
        unsigned long delay = status ? _relays[id].delay_on : _relays[id].delay_off;

        _relays[id].floodWindowChanges++;
        _relays[id].scheduledStatusTime = currentTime + delay;

        // If currentTime is off-limits the floodWindow...
        if (currentTime < _relays[id].floodWindowStart || floodWindowEnd <= currentTime) {

            // We reset the floodWindow
            _relays[id].floodWindowStart = currentTime;
            _relays[id].floodWindowChanges = 1;

        // If currentTime is in the floodWindow and there have been too many requests...
        } else if (_relays[id].floodWindowChanges >= RELAY_FLOOD_CHANGES) {

            // We schedule the changes to the end of the floodWindow
            // unless it's already delayed beyond that point
            if (floodWindowEnd - delay > currentTime) {
                _relays[id].scheduledStatusTime = floodWindowEnd;
            }

        }

        _relays[id].scheduled = true;
        _relays[id].scheduledStatus = status;
        if (report) _relays[id].scheduledReport = true;

        DEBUG_MSG_P(PSTR("[RELAY] #%d scheduled %s in %u ms\n"),
                id, status ? "ON" : "OFF",
                (_relays[id].scheduledStatusTime - currentTime));

        changed = true;

    #if TRACK_RELAY_STATUS
    }
    #endif

    return changed;
}

bool relayStatus(unsigned char id, bool status) {
    return relayStatus(id, status, true);
}

bool relayStatus(unsigned char id) {
    return relayProviderStatus(id);
}

void relaySync(unsigned char id) {

    if (_relays.size() > 1) {

        recursive = true;

        byte relaySync = getSetting("relaySync", RELAY_SYNC).toInt();
        bool status = relayStatus(id);

        // If RELAY_SYNC_SAME all relays should have the same state
        if (relaySync == RELAY_SYNC_SAME) {
            for (unsigned short i=0; i<_relays.size(); i++) {
                if (i != id) relayStatus(i, status);
            }

        // If NONE_OR_ONE or ONE and setting ON we should set OFF all the others
        } else if (status) {
            if (relaySync != RELAY_SYNC_ANY) {
                for (unsigned short i=0; i<_relays.size(); i++) {
                    if (i != id) relayStatus(i, false);
                }
            }

        // If ONLY_ONE and setting OFF we should set ON the other one
        } else {
            if (relaySync == RELAY_SYNC_ONE) {
                unsigned char i = (id + 1) % _relays.size();
                relayStatus(i, true);
            }
        }

        recursive = false;

    }

}

void relaySave() {
    unsigned char bit = 1;
    unsigned char mask = 0;
    for (unsigned int i=0; i < _relays.size(); i++) {
        if (relayStatus(i)) mask += bit;
        bit += bit;
    }
    EEPROM.write(EEPROM_RELAY_STATUS, mask);
    DEBUG_MSG_P(PSTR("[RELAY] Saving mask: %d\n"), mask);
    EEPROM.commit();
}

void relayRetrieve(bool invert) {
    recursive = true;
    unsigned char bit = 1;
    unsigned char mask = invert ? ~EEPROM.read(EEPROM_RELAY_STATUS) : EEPROM.read(EEPROM_RELAY_STATUS);
    DEBUG_MSG_P(PSTR("[RELAY] Retrieving mask: %d\n"), mask);
    for (unsigned int id=0; id < _relays.size(); id++) {
        _relays[id].scheduledStatus = ((mask & bit) == bit);
        _relays[id].scheduledReport = true;
        bit += bit;
    }
    if (invert) {
        EEPROM.write(EEPROM_RELAY_STATUS, mask);
        EEPROM.commit();
    }
    recursive = false;
}

void relayToggle(unsigned char id) {
    if (id >= _relays.size()) return;
    relayStatus(id, !relayStatus(id));
}

unsigned char relayCount() {
    return _relays.size();
}

//------------------------------------------------------------------------------
// REST API
//------------------------------------------------------------------------------

void relaySetupAPI() {

    // API entry points (protected with apikey)
    for (unsigned int relayID=0; relayID<relayCount(); relayID++) {

        char url[15];
        sprintf(url, "%s/%d", MQTT_TOPIC_RELAY, relayID);

        char key[10];
        sprintf(key, "%s%d", MQTT_TOPIC_RELAY, relayID);

        apiRegister(url, key,
            [relayID](char * buffer, size_t len) {
				snprintf(buffer, len, "%d", relayStatus(relayID) ? 1 : 0);
            },
            [relayID](const char * payload) {
                unsigned int value = payload[0] - '0';
                if (value == 2) {
                    relayToggle(relayID);
                } else {
                    relayStatus(relayID, value == 1);
                }
            }
        );

    }

}

//------------------------------------------------------------------------------
// WebSockets
//------------------------------------------------------------------------------

void relayWS() {
    DynamicJsonBuffer jsonBuffer;
    JsonObject& root = jsonBuffer.createObject();
    JsonArray& relay = root.createNestedArray("relayStatus");
    for (unsigned char i=0; i<relayCount(); i++) {
        relay.add(relayStatus(i));
    }
    String output;
    root.printTo(output);
    wsSend(output.c_str());
}

//------------------------------------------------------------------------------
// MQTT
//------------------------------------------------------------------------------

void relayMQTT(unsigned char id) {
    if (id >= _relays.size()) return;
    mqttSend(MQTT_TOPIC_RELAY, id, relayStatus(id) ? "1" : "0");
}

void relayMQTT() {
    for (unsigned int i=0; i < _relays.size(); i++) {
        relayMQTT(i);
    }
}

void relayMQTTCallback(unsigned int type, const char * topic, const char * payload) {

    if (type == MQTT_CONNECT_EVENT) {

        #if not HEARTBEAT_REPORT_RELAY
            relayMQTT();
        #endif

        char buffer[strlen(MQTT_TOPIC_RELAY) + 3];
        sprintf(buffer, "%s/+", MQTT_TOPIC_RELAY);
        mqttSubscribe(buffer);

    }

    if (type == MQTT_MESSAGE_EVENT) {

        // Match topic
        String t = mqttSubtopic((char *) topic);
        if (!t.startsWith(MQTT_TOPIC_RELAY)) return;

        // Get value
        unsigned int value = (char)payload[0] - '0';

        // Pulse topic
        if (t.endsWith("pulse")) {
            relayPulseMode(value, mqttForward());
            return;
        }

        // Get relay ID
        unsigned int relayID = t.substring(strlen(MQTT_TOPIC_RELAY)+1).toInt();
        if (relayID >= relayCount()) {
            DEBUG_MSG_P(PSTR("[RELAY] Wrong relayID (%d)\n"), relayID);
            return;
        }

        // Action to perform
        if (value == 2) {
            relayToggle(relayID);
        } else {
            relayStatus(relayID, value > 0, mqttForward());
        }

    }

}

void relaySetupMQTT() {
    mqttRegister(relayMQTTCallback);
}

//------------------------------------------------------------------------------
// InfluxDB
//------------------------------------------------------------------------------

#if ENABLE_INFLUXDB
void relayInfluxDB(unsigned char id) {
    if (id >= _relays.size()) return;
    char buffer[10];
    sprintf(buffer, "%s,id=%d", MQTT_TOPIC_RELAY, id);
    influxDBSend(buffer, relayStatus(id) ? "1" : "0");
}
#endif

//------------------------------------------------------------------------------
// Setup
//------------------------------------------------------------------------------

void relaySetup() {

    // Dummy relays for AI Light, Magic Home LED Controller, H801,
    // Sonoff Dual and Sonoff RF Bridge
    #ifdef DUMMY_RELAY_COUNT

        for (unsigned char i=0; i < DUMMY_RELAY_COUNT; i++) {
            _relays.push_back((relay_t) {0, 0});
            _relays[i].scheduled = false;
        }

    #else

        #ifdef RELAY1_PIN
            _relays.push_back((relay_t) { RELAY1_PIN, RELAY1_PIN_INVERSE, RELAY1_LED, RELAY1_DELAY_ON, RELAY1_DELAY_OFF });
        #endif
        #ifdef RELAY2_PIN
            _relays.push_back((relay_t) { RELAY2_PIN, RELAY2_PIN_INVERSE, RELAY2_LED, RELAY2_DELAY_ON, RELAY2_DELAY_OFF });
        #endif
        #ifdef RELAY3_PIN
            _relays.push_back((relay_t) { RELAY3_PIN, RELAY3_PIN_INVERSE, RELAY3_LED, RELAY3_DELAY_ON, RELAY3_DELAY_OFF });
        #endif
        #ifdef RELAY4_PIN
            _relays.push_back((relay_t) { RELAY4_PIN, RELAY4_PIN_INVERSE, RELAY4_LED, RELAY4_DELAY_ON, RELAY4_DELAY_OFF });
        #endif

    #endif

    byte relayMode = getSetting("relayMode", RELAY_MODE).toInt();
    for (unsigned int i=0; i < _relays.size(); i++) {
        pinMode(_relays[i].pin, OUTPUT);
        if (relayMode == RELAY_MODE_OFF) relayStatus(i, false);
        if (relayMode == RELAY_MODE_ON) relayStatus(i, true);
    }
    if (relayMode == RELAY_MODE_SAME) relayRetrieve(false);
    if (relayMode == RELAY_MODE_TOOGLE) relayRetrieve(true);
    relayLoop();

    relaySetupAPI();
    relaySetupMQTT();

    DEBUG_MSG_P(PSTR("[RELAY] Number of relays: %d\n"), _relays.size());

}

void relayLoop(void) {

    unsigned char id;

    for (id = 0; id < _relays.size(); id++) {

        unsigned int currentTime = millis();
        bool status = _relays[id].scheduledStatus;

        #if TRACK_RELAY_STATUS
        if (relayStatus(id) != status && currentTime >= _relays[id].scheduledStatusTime) {
        #else
        if (_relays[id].scheduled && currentTime >= _relays[id].scheduledStatusTime) {
        #endif

            DEBUG_MSG_P(PSTR("[RELAY] #%d set to %s\n"), id, status ? "ON" : "OFF");

            // Call the provider to perform the action
            relayProviderStatus(id, status);

            // Change the binded LED if any
            if (_relays[id].led > 0) {
                ledStatus(_relays[id].led - 1, status);
            }

            // Send MQTT report if requested
            if (_relays[id].scheduledReport) {
                relayMQTT(id);
            }

            if (!recursive) {
                relayPulse(id);
                relaySync(id);
                relaySave();
                relayWS();
            }

            #if ENABLE_DOMOTICZ
                relayDomoticzSend(id);
            #endif

            #if ENABLE_INFLUXDB
                relayInfluxDB(id);
            #endif

            _relays[id].scheduled = false;
            _relays[id].scheduledReport = false;

        }

    }

}
