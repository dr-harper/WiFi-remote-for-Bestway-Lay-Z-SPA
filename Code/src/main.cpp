#include "main.h"
#include "ports.h"
#include "webauth.h"
#include "logbuf.h"

#define WS_PERIOD 4.0
// initial stack
char *stack_start;
uint32_t heap_water_mark;

// Setup a oneWire instance to communicate with any OneWire devices
// Setting arbitrarily to 231 since this isn't an actual pin
// Later during "setup" the correct pin will be set, if enabled 
OneWire *oneWire;
// Pass our oneWire reference to Dallas Temperature sensor 
DallasTemperature *tempSensors;

WiFiEventHandler gotIpEventHandler, disconnectedEventHandler;
void cb_gotIP(const WiFiEventStationModeGotIP& event)
{
    gotIP_flag = true;
    logbuf::writef("WiFi > GOT IP %s  gw=%s  rssi=%d  ssid='%s'  bssid=%s  ch=%d",
                   event.ip.toString().c_str(),
                   event.gw.toString().c_str(),
                   WiFi.RSSI(),
                   WiFi.SSID().c_str(),
                   WiFi.BSSIDstr().c_str(),
                   WiFi.channel());
}

void gotIP()
{
    ESP.wdtFeed();
    BWC_LOG_P(PSTR("start of gotip millis = %d\n"), millis());
    gotIP_flag = false;
    WiFi.softAPdisconnect();
    WiFi.mode(WIFI_STA);
    BWC_LOG_P(PSTR("Soft AP > closed\n"), 0);
    BWC_LOG_P(PSTR("Connected as station with localIP: %s\n"), WiFi.localIP().toString().c_str());
    startNTP();
    BWC_LOG_P(PSTR("end of gotip millis = %d\n"), millis());
    bwc->print(WiFi.localIP().toString());
    if(mqtt_info->useMqtt) mqttConnect();
    BWC_YIELD;
}

// Decode WIFI_DISCONNECT_REASON_* enum values to short labels for the
// log buffer. Lifted from the ESP8266 SDK header — the most useful
// codes for diagnosing join failures are around 200-215 (auth fail,
// timeout, beacon loss).
static const char *disconnectReasonStr(uint8_t r) {
    switch (r) {
        case 1:   return "UNSPECIFIED";
        case 2:   return "AUTH_EXPIRE";
        case 3:   return "AUTH_LEAVE";
        case 4:   return "ASSOC_EXPIRE";
        case 5:   return "ASSOC_TOOMANY";
        case 6:   return "NOT_AUTHED";
        case 7:   return "NOT_ASSOCED";
        case 8:   return "ASSOC_LEAVE";
        case 200: return "BEACON_TIMEOUT";
        case 201: return "NO_AP_FOUND";
        case 202: return "AUTH_FAIL";        // wrong password
        case 203: return "ASSOC_FAIL";
        case 204: return "HANDSHAKE_TIMEOUT"; // WPA2 4-way handshake fail
        default:  return "?";
    }
}

void cb_disconnected(const WiFiEventStationModeDisconnected& event)
{
    disconnected_flag = true;
    logbuf::writef("WiFi > DISCONNECTED  ssid='%s'  reason=%u (%s)",
                   event.ssid.c_str(),
                   (unsigned)event.reason,
                   disconnectReasonStr((uint8_t)event.reason));
    // startSoftAp();
}

void setup()
{
    
    // init record of stack
    char stack;
    stack_start = &stack;

    Serial.begin(76800);
    BWC_LOG_P(PSTR("\nSetup > Start @ millis: %d\n"),millis());
    /*register wifi events */
    gotIpEventHandler = WiFi.onStationModeGotIP(cb_gotIP);
    disconnectedEventHandler = WiFi.onStationModeDisconnected(cb_disconnected);

    LittleFS.begin();
    // Auth must be initialised AFTER LittleFS.begin() — it reads
    // /auth.json for the stored password + session token, falling back
    // to defaults on first boot. See webauth.h for the contract.
    webauth::setup();
    logbuf::setup();
    // Reset reason makes wedge-recoveries visible in /logs.html — a
    // "Software/System restart" boot following an "External System" is
    // the soft watchdog firing; "Power-on" is a normal cold boot.
    logbuf::writef("=== BOOT === freeHeap=%u, maxBlock=%u, FW=%s, resetReason='%s'",
                   ESP.getFreeHeap(), ESP.getMaxFreeBlockSize(),
                   FW_VERSION, ESP.getResetReason().c_str());
    {
        HeapSelectIram ephemeral;
        bwc = new BWC;
        oneWire = new OneWire(231);
        tempSensors = new DallasTemperature(oneWire);
        // bootlogTimer = new Ticker;
        periodicTimer = new Ticker;
        startComplete_ticker = new Ticker;
        ntpCheck_ticker = new Ticker;
        // checkWifi_ticker = new Ticker;
        updateWSTimer = new Ticker;
        updateMqttTimer = new Ticker;
        mqtt_info = new sMQTT_info;
        mqtt_info->mqttBaseTopic = MQTT_BASE_TOPIC_F;
        mqtt_info->mqttClientId = MQTT_CLIENT_ID_F;
        mqtt_info->mqttHost = MQTT_HOST_F;
        mqtt_info->mqttPassword = MQTT_PASSWORD_F;
        mqtt_info->mqttPort = MQTT_PORT;
        mqtt_info->mqttTelemetryInterval = MQTT_TELEMETRY_INTERVAL;
        mqtt_info->mqttUsername = MQTT_USER_F;
        mqtt_info->useMqtt = MQTT_USEMQTT;
        wifi_info = new sWifi_info{.enableWmApFallback = true};
    }
    bwc->setup();
    bwc->loop();
    periodicTimer->attach(periodicTimerInterval, []{ periodicTimerFlag = true; });
    // delayed mqtt start
    startComplete_ticker->attach(30, []{ bwc->restoreStates(); startComplete_ticker->detach(); delete startComplete_ticker; }); //can it destroy itself?
    // update webpage every WS_PERIOD seconds. (will also be updated on state changes)
    updateWSTimer->attach(WS_PERIOD, []{ sendWSFlag = true; });
    loadWebConfig();
    startWiFi();
    if(wifi_info->enableWmApFallback) startSoftAp(); // not blocking anymore so no use case should exist for this to be turned off.
    startHttpServer();
    startWebSocket();
    startOTA();
    startMqtt();
    if(bwc->hasTempSensor)
    { 
        HeapSelectIram ephemeral;
        oneWire->begin(bwc->tempSensorPin);
        tempSensors->begin();
    }
    bwc->print("---");  //No overloaded function exists for the F() macro
    bwc->print(FW_VERSION);
    BWC_LOG_P(PSTR("End of setup() @ Millis: %d @ line: %d. Heap: %d\n"), millis(), __LINE__, ESP.getFreeHeap());
    heap_water_mark = ESP.getFreeHeap();

    // Boot watchdog temporarily disabled while debugging a setup-time
    // hang. Re-enable once root cause is found.
    // bootGuardTicker = new Ticker;
    // bootGuardTicker->once(BOOT_GUARD_SECONDS, []{ bootGuardFiredFlag = true; });
}

// Soft watchdog — auto-recovers from the wedge state where WiFi stays up
// but async-TCP buffers are exhausted (HTTP thread alive but unreachable).
// Detection: track when max-contiguous-free-block first dropped below the
// threshold; if it stays there for the timeout AND the device has been
// up long enough that we trust the reading, ESP.restart(). A soft restart
// preserves OTA partition state + LittleFS contents; only MQTT/websocket
// sessions reconnect (which they do on any transient disconnect anyway).
//
// Why these specific numbers (chosen 2026-06-01 after wedging on a burst):
//  · 6 KB threshold — healthy idle is ~30 KB; "busy but fine" is ~10–15 KB;
//    only true fragmentation drops below 6 KB. ~80% drop from baseline.
//  · 30 s sustain — transient busy spikes finish in seconds; a wedge
//    persists indefinitely. 30 s separates them cleanly.
//  · 5 min uptime guard — onboarding + first NTP sync + initial MQTT
//    handshake can briefly fragment heap. The guard lets these settle
//    before the watchdog is allowed to fire.
//
// State exposed via /api/watchdog-status for runtime auditing.
static const uint32_t  WATCHDOG_MAXBLOCK_THRESHOLD = 6000;        // bytes
static const unsigned long WATCHDOG_LOW_HEAP_TIMEOUT = 30000UL;    // ms
static const unsigned long WATCHDOG_MIN_UPTIME = 5UL * 60UL * 1000UL;  // ms

static unsigned long lowHeapStartedMs = 0;   // when current arm-period began (0 = disarmed)
static unsigned long wdArmCount = 0;         // how many times we've armed this boot
static unsigned long wdLastArmedAtMs = 0;    // millis() of most recent arm event
static uint32_t      wdLastArmedMaxBlock = 0;// snapshot at most recent arm

void loop()
{
    uint32_t freeheap = ESP.getFreeHeap();
    if(freeheap < heap_water_mark) heap_water_mark = freeheap;

    // Soft watchdog tick. Cheap: two ESP-API reads + small constant logic.
    // Logs WARN on first arm, INFO on disarm, ERROR + restart on fire.
    uint32_t maxBlock = ESP.getMaxFreeBlockSize();
    if (maxBlock < WATCHDOG_MAXBLOCK_THRESHOLD) {
        if (lowHeapStartedMs == 0) {
            lowHeapStartedMs = millis();
            wdLastArmedAtMs = lowHeapStartedMs;
            wdLastArmedMaxBlock = maxBlock;
            wdArmCount++;
            logbuf::writef("WATCHDOG arm #%lu: maxBlock=%u B below %u — uptime=%lu s, allowed to fire in %lu s if heap doesn't recover",
                           wdArmCount, maxBlock, WATCHDOG_MAXBLOCK_THRESHOLD,
                           millis() / 1000UL,
                           WATCHDOG_LOW_HEAP_TIMEOUT / 1000UL);
        } else if (millis() - lowHeapStartedMs > WATCHDOG_LOW_HEAP_TIMEOUT) {
            // Uptime guard: don't fire during the first WATCHDOG_MIN_UPTIME ms
            // since boot. Onboarding + NTP + first MQTT handshake can briefly
            // fragment heap; we treat <5 min as not-yet-trustworthy.
            if (millis() < WATCHDOG_MIN_UPTIME) {
                static bool warnedGuard = false;
                if (!warnedGuard) {
                    logbuf::writef("WATCHDOG suppressed: would fire but uptime %lu s < min %lu s (boot-warmup guard)",
                                   millis() / 1000UL, WATCHDOG_MIN_UPTIME / 1000UL);
                    warnedGuard = true;
                }
            } else {
                logbuf::writef("WATCHDOG fire #%lu: heap fragmented %lu s (maxBlock=%u B, free=%u B, uptime=%lu s) — ESP.restart()",
                               wdArmCount, (millis() - lowHeapStartedMs) / 1000UL,
                               maxBlock, freeheap, millis() / 1000UL);
                delay(150);  // let logbuf flush before restart
                ESP.restart();
            }
        }
    } else if (lowHeapStartedMs != 0) {
        logbuf::writef("WATCHDOG disarm #%lu: heap recovered (maxBlock=%u B after %lu s)",
                       wdArmCount, maxBlock, (millis() - lowHeapStartedMs) / 1000UL);
        lowHeapStartedMs = 0;
    }


    if(gotIP_flag) gotIP();
    if(disconnected_flag) startSoftAp();

    // Boot watchdog disabled while debugging setup-time hang.
    // We need this self-destructing info several times, so save it on the stack
    bool newData = bwc->newData();
    // Fiddle with the pump computer
    bwc->loop();

    // listen for webserver events
    if(server){
        server->handleClient();
        // Serial.print(".");
    }
    // listen for OTA events
    ArduinoOTA.handle();
    // web socket
    if (newData || sendWSFlag)
    {
        sendWSFlag = false;
        sendWS();
    }
    // run only when a wifi connection is established
    /* MQTT, OTA & NTP is not relevant in softAP mode */
    if (WiFi.status() == WL_CONNECTED)
    {

        // MQTT
        if (mqtt_info->useMqtt && mqttClient->loop())
        {
            String msg;
            msg.reserve(32);
            bwc->getButtonName(msg);
            // publish pretty button name if display button is pressed (or NOBTN if released)
            if (!msg.equals(prevButtonName))
            {
                mqttClient->publish((String(mqtt_info->mqttBaseTopic) + "/button").c_str(), String(msg).c_str(), true);
                prevButtonName = msg;
            }
            if (newData || sendMQTTFlag)
            {
                sendMQTT();
                sendMQTTFlag = false;
            }
            if(send_mqtt_cfg_needed)
            {
                send_mqtt_cfg_needed = false;
                sendMQTTConfig();
            }
        }

        if(checkNTP_flag)
        {
            checkNTP_flag = false;
            checkNTP();
        }
    }

    // run every X seconds
    if (periodicTimerFlag)
    {
        periodicTimerFlag = false;
        if(WiFi.getMode() == WIFI_AP_STA)
        {
            wifi_manual_reconnect();
        }
        if (mqtt_info->useMqtt && !mqttClient->loop() && (WiFi.status() == WL_CONNECTED))
        {
            BWC_LOG_P(PSTR("MQTT > Not connected\n"),0);
            mqttConnect();
        }
        // Leverage the pre-existing periodicTimerFlag to also set temperature, if enabled
        setTemperatureFromSensor();
    }

    //Only do this if locked out! (by pressing POWER - LOCK - TIMER - POWER)
    if(bwc->getBtnSeqMatch())
    {   
        resetWiFi();
        delay(3000);
        ESP.reset();
        delay(3000);
    }
}


/**
 * Send status data to web client in JSON format (because it is easy to decode on the other side)
 */
void sendWS()
{
    if(!webSocket) return;
    if(webSocket->connectedClients() == 0) return;
    HeapSelectIram ephemeral;
    // Serial.printf("IRamheap %d\n", ESP.getFreeHeap());
    // send states
    String json;
    json.reserve(384);

    bwc->getJSONStates(json);
    webSocket->broadcastTXT(json);
    // send times
    json.clear();
    bwc->getJSONTimes(json);
    webSocket->broadcastTXT(json);
    // send other info
    json.clear();
    getOtherInfo(json);
    webSocket->broadcastTXT(json);
    // json = bwc->getDebugData();
    // webSocket->broadcastTXT(json);
    // time_t now = time(nullptr);
    // struct tm timeinfo;
    // gmtime_r(&now, &timeinfo);
    // Serial.print("Current time: ");
    // Serial.print(asctime(&timeinfo));
    BWC_YIELD;
}

void getOtherInfo(String &rtn)
{
    // DynamicJsonDocument doc(512);
    StaticJsonDocument<512> doc;
    // Set the values in the document
    doc[F("CONTENT")] = F("OTHER");
    doc[F("MQTT")] = mqttClient->state();
    /*TODO: add these:*/
    //   doc[F("PressedButton")] = bwc->getPressedButton();
    doc[F("HASJETS")] = bwc->hasjets;
    doc[F("HASGOD")] = bwc->hasgod;
    doc[F("MODEL")] = bwc->getModel();
    doc[F("RSSI")] = WiFi.RSSI();
    doc[F("IP")] = WiFi.localIP().toString();
    doc[F("SSID")] = WiFi.SSID();
    doc[F("FW")] = FW_VERSION;
    doc[F("loopfq")] = bwc->loop_count;
    bwc->loop_count = 0;

    // Serialize JSON to string
    if (serializeJson(doc, rtn) == 0)
    {
        rtn = F("{\"error\": \"Failed to serialize other\"}");
    }
    BWC_YIELD;
}

/**
 * Send STATES and TIMES to MQTT
 * It would be more elegant to send both states and times on the "message" topic
 * and use the "CONTENT" field to distinguish between them
 * but it might break peoples home automation setups, so to keep it backwards
 * compatible I choose to start a new topic "/times"
 * @author 877dev
 */
void sendMQTT()
{
    HeapSelectIram ephemeral;
    // Serial.printf("IRamheap %d\n", ESP.getFreeHeap());
    String json;
    json.reserve(320);

    // send states
    bwc->getJSONStates(json);
    if (mqttClient->publish((String(mqtt_info->mqttBaseTopic) + F("/message")).c_str(), String(json).c_str(), true))
    {
        BWC_LOG_P(PSTR("MQTT > message published\n"),0);
    }
    else
    {
        BWC_LOG_P(PSTR("MQTT > message not published\n"),0);
    }

    // send times
    json.clear();
    bwc->getJSONTimes(json);
    if (mqttClient->publish((String(mqtt_info->mqttBaseTopic) + F("/times")).c_str(), String(json).c_str(), true))
    {
        BWC_LOG_P(PSTR("MQTT > times published\n"),0);
    }
    else
    {
        BWC_LOG_P(PSTR("MQTT > times not published\n"),0);
    }

    //send other info
    json.clear();
    getOtherInfo(json);
    if (mqttClient->publish((String(mqtt_info->mqttBaseTopic) + F("/other")).c_str(), String(json).c_str(), true))
    {
        BWC_LOG_P(PSTR("MQTT > other published\n"),0);
    }
    else
    {
        BWC_LOG_P(PSTR("MQTT > other not published\n"),0);
    }
    BWC_YIELD;
}

void sendMQTTConfig()
{
    BWC_LOG_P(PSTR("MQTT > sending config\n"),0);
    String json;
    json.reserve(320);
    bwc->getJSONSettings(json);
    mqttClient->publish((String(mqtt_info->mqttBaseTopic) + F("/get_config")).c_str(), String(json).c_str(), true);
    mqttClient->loop();
    BWC_YIELD;
}

/**
 * Start a Wi-Fi access point, and try to connect to some given access points.
 * Then wait for either an AP or STA connection
 */
void startWiFi()
{
    BWC_LOG_P(PSTR("startWiFi() @ millis: %d\n"), millis());
    //WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.persistent(true);
    WiFi.hostname(DEVICE_NAME_F);
    WiFi.mode(WIFI_STA); //WiFi.setOutputPower(15.0);
    loadWifi();

    if (wifi_info->enableStaticIp4)
    {
        BWC_LOG_P(PSTR("Setting static IP\n"),0);
        IPAddress ip4Address;
        IPAddress ip4Gateway;
        IPAddress ip4Subnet;
        IPAddress ip4DnsPrimary;
        IPAddress ip4DnsSecondary;
        ip4Address.fromString(wifi_info->ip4Address_str);
        ip4Gateway.fromString(wifi_info->ip4Gateway_str);
        ip4Subnet.fromString(wifi_info->ip4Subnet_str);
        ip4DnsPrimary.fromString(wifi_info->ip4DnsPrimary_str);
        ip4DnsSecondary.fromString(wifi_info->ip4DnsSecondary_str);
        BWC_LOG_P(PSTR("WiFi > using static IP %s on gateway %s\n"),ip4Address.toString().c_str(), ip4Gateway.toString().c_str());
        WiFi.config(ip4Address, ip4Gateway, ip4Subnet, ip4DnsPrimary, ip4DnsSecondary);
    }

    wifi_manual_reconnect();
    BWC_YIELD;
}

// Parse a BSSID string like "AA:BB:CC:DD:EE:FF" into a 6-byte buffer.
// Returns true on success; on failure leaves the buffer untouched and
// the caller falls back to non-pinned WiFi.begin().
static bool parseBssidString(const String &bssidStr, uint8_t out[6])
{
    if (bssidStr.length() != 17) return false;
    for (int i = 0; i < 6; i++) {
        int hi = i * 3;
        int lo = hi + 1;
        char c0 = bssidStr[hi];
        char c1 = bssidStr[lo];
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int n0 = nib(c0), n1 = nib(c1);
        if (n0 < 0 || n1 < 0) return false;
        out[i] = (uint8_t)((n0 << 4) | n1);
    }
    return true;
}

void wifi_manual_reconnect()
{
    // ESP8266 workaround stack for picky APs (Vodafone Ultrahub Smart
    // Connect etc.). Each knob is low-yield alone, combined they often
    // unlock association.
    //
    //   1. WiFi.persistent(false) — don't write creds to flash via the
    //      SDK; avoids stale-state retry loops on failed attempts
    //   2. WiFi.disconnect() — drop any stale association (no `true`
    //      arg — that would also power-cycle the modem, and the
    //      following setPhyMode/setSleepMode on a powered-off modem
    //      crashed the device on this ESP8266 core. Default disconnect
    //      keeps the modem on, which is what we want)
    //   3. WiFi.setPhyMode(WIFI_PHY_MODE_11G) — skip 11n MCS rate
    //      negotiation, which is buggy on ESP8266 vs newer APs
    //   4. WiFi.setSleepMode(WIFI_NONE_SLEEP) — keep radio fully awake;
    //      some APs deauth a STA that goes to power-save too quickly
    WiFi.persistent(false);
    WiFi.disconnect();
    delay(100);
    WiFi.setPhyMode(WIFI_PHY_MODE_11G);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);

    /* Connect in station mode to the AP given (your router/ap) */
    if (wifi_info->enableAp)
    {
        BWC_LOG_P(PSTR("WiFi > using WiFi configuration with SSID %s\n"), wifi_info->apSsid.c_str());

        // BSSID pin — if the user has selected a specific AP MAC from
        // the scan UI (mesh-router case), use the 4-arg WiFi.begin so
        // the driver doesn't roam to a worse-RSSI AP that shares the
        // SSID. channel=0 means "auto-detect channel for this BSSID."
        uint8_t bssid[6];
        if (wifi_info->apBssid.length() == 17 &&
            parseBssidString(wifi_info->apBssid, bssid))
        {
            BWC_LOG_P(PSTR("WiFi > pinning to BSSID %s\n"), wifi_info->apBssid.c_str());
            logbuf::writef("WiFi > BEGIN ssid='%s' bssid=%s pwd_len=%u  heap=%u  mode=11G  sleep=NONE",
                           wifi_info->apSsid.c_str(),
                           wifi_info->apBssid.c_str(),
                           (unsigned)wifi_info->apPwd.length(),
                           ESP.getFreeHeap());
            WiFi.begin(wifi_info->apSsid.c_str(),
                       wifi_info->apPwd.c_str(),
                       0,        // channel — 0 = auto
                       bssid,
                       true);    // connect immediately
        }
        else
        {
            logbuf::writef("WiFi > BEGIN ssid='%s' bssid=(auto) pwd_len=%u  heap=%u  mode=11G  sleep=NONE",
                           wifi_info->apSsid.c_str(),
                           (unsigned)wifi_info->apPwd.length(),
                           ESP.getFreeHeap());
            WiFi.begin(wifi_info->apSsid.c_str(), wifi_info->apPwd.c_str());
        }
        // checkWifi_ticker->attach(2.0, checkWiFi_ISR);
        BWC_LOG_P(PSTR("WiFi > AP info loaded. Waiting for connection ...\n"), 0);
    }
    else
    {
        BWC_LOG_P(PSTR("WiFi > AP info not found. Using last known AP ...\n"), 0);
        WiFi.begin();
    }
}

/**
 * start WiFiManager configuration portal
 */
void startSoftAp()
{
    disconnected_flag = false;
    if(WiFi.getMode() == WIFI_AP_STA) {
        BWC_LOG_P(PSTR("Soft AP IP: %s.\n"),WiFi.softAPIP().toString().c_str());
        return;
    }
    BWC_LOG_P(PSTR("Station > disconnected. Starting soft AP\n"),0);
    bwc->print(F("check network"));
    WiFi.mode(WIFI_AP_STA);
    IPAddress local_IP(192,168,4,2);
    IPAddress gateway(192,168,4,1);
    IPAddress subnet(255,255,255,0);
    BWC_LOG_P(PSTR("WiFi > soft-AP configuration: %s\n"),WiFi.softAPConfig(local_IP, gateway, subnet) ? "OK" : "Failed!");
    BWC_LOG_P(PSTR("WiFi > soft AP mode: %s\n"),WiFi.softAP(WM_AP_NAME_F, WM_AP_PASSWORD_F)?"OK": "SoftAP fail");
    BWC_LOG_P(PSTR("WiFi > Soft AP IP: %s\n"),WiFi.softAPIP().toString().c_str());
    BWC_YIELD;
}

void checkNTP_ISR()
{
    checkNTP_flag = true;
}

void checkNTP()
{
    time_t now = time(nullptr);
    // static uint8_t ntpTryNumber = 0;
    if(now < 57600)
    {
        // if (++ntpTryNumber == 10) {
        //     ntpTryNumber = 0; //reset until next check
        //     ntpCheck_ticker->detach(); //give up. Next check won't happen.
        // }
        return;
    }
    ntpCheck_ticker->detach(); //time is set, don't check again
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    time_t boot_timestamp = getBootTime();
    tm * boot_time_tm = gmtime(&boot_timestamp);
    char boot_time_str[64];
    strftime(boot_time_str, 64, "%F %T", boot_time_tm);
    bwc->reboot_time_str = String(boot_time_str);
    bwc->reboot_time_t = boot_timestamp;
    if(firstNtpSyncAfterBoot)
    {
        BWC_LOG_P(PSTR("NTP > synced: %s. Saving boot info.\n"),bwc->reboot_time_str.c_str());
        firstNtpSyncAfterBoot = false;
        bwc->saveRebootInfo();
    }
    BWC_YIELD;
}

/**
 * start NTP sync
 */
void startNTP()
{
    BWC_LOG_P(PSTR("NTP > start\n"),0);
    configTime(0,0,wifi_info->ip4NTP_str, F("pool.ntp.org"), F("time.nist.gov"));
    ntpCheck_ticker->attach(3.0, checkNTP_ISR);
}

void startOTA()
{
    BWC_LOG_P(PSTR("OTA > start\n"),0);
    String dname = DEVICE_NAME_F;
    String pw = OTA_PSWD_F;
    ArduinoOTA.setHostname(dname.c_str());
    ArduinoOTA.setPassword(pw.c_str());

    ArduinoOTA.onStart([]() {
        // Serial.println(F("OTA > Start"));
        stopall();
    });
    ArduinoOTA.onEnd([]() {
        // Serial.println(F("OTA > End"));
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        // Serial.printf("OTA > Progress: %u%%\r\n", (progress / (total / 100)));
    });
    ArduinoOTA.onError([](ota_error_t error) {
        // Serial.printf("OTA > Error[%u]: ", error);
        // if (error == OTA_AUTH_ERROR) Serial.println(F("Auth Failed"));
        // else if (error == OTA_BEGIN_ERROR) Serial.println(F("Begin Failed"));
        // else if (error == OTA_CONNECT_ERROR) Serial.println(F("Connect Failed"));
        // else if (error == OTA_RECEIVE_ERROR) Serial.println(F("Receive Failed"));
        // else if (error == OTA_END_ERROR) Serial.println(F("End Failed"));
    });
    ArduinoOTA.begin();
    // Serial.println(F("OTA > ready"));
}

void stopall()
{
    BWC_LOG_P(PSTR("Stop all > Free mem before stop: %d\n"), ESP.getFreeHeap());
    bwc->stop();
    BWC_LOG_P(PSTR("MQTT > detaching\n"),0);
    updateMqttTimer->detach();
    BWC_LOG_P(PSTR("Periodic timer > detaching\n"),0);
    periodicTimer->detach();
    BWC_LOG_P(PSTR("WS > detaching\n"),0);
    updateWSTimer->detach();
    if(ntpCheck_ticker->active()) ntpCheck_ticker->detach();
    // if(checkWifi_ticker->active()) checkWifi_ticker->detach();
    //bwc->saveSettings();
    delete tempSensors;
    delete oneWire;
    BWC_LOG_P(PSTR("MQTT > stopping\n"),0);
    if(mqtt_info->useMqtt) mqttClient->disconnect();
    if(aWifiClient) delete aWifiClient;
    aWifiClient = nullptr;
    // delete mqttClient; //Compiler nagging about not deleting virtual classes.
    mqttClient = nullptr;
    BWC_LOG_P(PSTR("HTTPServer > stopping\n"),0);
    server->stop();
    delete server;
    server = nullptr;
    BWC_LOG_P(PSTR("WS > stopping\n"),0);
    webSocket->close();
    delete webSocket;
    webSocket = nullptr;
    BWC_LOG_P(PSTR("FS > stopping\n"),0);
    LittleFS.end();
    BWC_LOG_P(PSTR("Stop all > done. Free mem: %d\n"), ESP.getFreeHeap());
}

/*pause: action=true cont: action=false*/
void pause_all(bool action)
{
    if(action)
    {
        if(periodicTimer->active()) periodicTimer->detach();
        // if(startComplete_ticker->active()) startComplete_ticker->detach();
        if(updateWSTimer->active()) updateWSTimer->detach();
        // if(bootlogTimer->active()) bootlogTimer->detach();
        if(ntpCheck_ticker->active()) ntpCheck_ticker->detach();
    } else 
    {
        periodicTimer->attach(periodicTimerInterval, []{ periodicTimerFlag = true; });
        // startComplete_ticker->attach(60, []{ if(mqtt_info->useMqtt) enableMqtt = true; startComplete_ticker->detach(); });
        updateWSTimer->attach(WS_PERIOD, []{ sendWSFlag = true; });
        //bootlogTimer.attach(5, []{ if(DateTime.isTimeValid()) {bwc->saveRebootInfo(); bootlogTimer.detach();} });
    }
    bwc->pause_all(action);
}

void startWebSocket()
{
    HeapSelectIram ephemeral;
    BWC_LOG_P(PSTR("WS > start. IRam heap: %d\n"), ESP.getFreeHeap());
    if(webSocket != nullptr)
    {
        webSocket->disconnect();
        webSocket->close();
        delete webSocket;
        webSocket = nullptr;
    }
    webSocket = new WebSocketsServer(81);
    webSocket->begin();
    // webSocket->enableHeartbeat(11000, 5000, 2);
    webSocket->onEvent(webSocketEvent);
    // Serial.println(F("WebSocket > server started"));
}

/**
 * handle web socket events
 */
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t len)
{
    // When a WebSocket message is received
    switch (type)
    {
        // if the websocket is disconnected
        case WStype_DISCONNECTED:
            BWC_LOG_P(PSTR("WS > [%u] Disconnected!\n"), num);
        break;

        // if a new websocket connection is established
        case WStype_CONNECTED:
        {
            // IPAddress ip = webSocket->remoteIP(num);
            // Serial.printf("WebSocket > [%u] Connected from %d.%d.%d.%d url: %s\r\n", num, ip[0], ip[1], ip[2], ip[3], payload);
            sendWS();
        }
        break;

        // if new text data is received
        case WStype_TEXT:
        {
            // Serial.printf("WebSocket > [%u] get Text: %s\r\n", num, payload);
            // DynamicJsonDocument doc(256);
            StaticJsonDocument<256> doc;
            DeserializationError error = deserializeJson(doc, payload);
            if (error)
            {
                BWC_LOG_P(PSTR("WS > JSON command failed"),0);
                return;
            }

            // Copy values from the JsonDocument to the Config
            Commands command = doc[F("CMD")];
            int64_t value = doc[F("VALUE")];
            int64_t xtime = doc[F("XTIME")];
            int64_t interval = doc[F("INTERVAL")];
            String txt = doc[F("TXT")] | "";
            command_que_item item;
            item.cmd = command;
            item.val = value;
            item.xtime = xtime;
            item.interval = interval;
            item.text = txt;
            bwc->add_command(item);
        }
        break;

        default:
            BWC_LOG_P(PSTR("WebSocket > Type:[%u]\r\n"), (unsigned int)type);
        break;
    }
}

/**
 * start a HTTP server with a file read and upload handler
 */
void startHttpServer()
{
    BWC_LOG_P(PSTR("HTTP Server > start/restart.\n"),0);
    if(server != nullptr)
    {
        server->stop();
        server->close();
        delete server;
        server = nullptr;
    }

    {
        // HeapSelectIram ephemeral;
        server = new ESP8266WebServer(80);
        // ESP8266WebServer only retains a few standard request headers
        // by default (Host etc.) — anything else read via hasHeader()
        // returns false unless explicitly registered here. Cookie is
        // load-bearing for the auth flow in webauth.cpp; without this
        // call the auth gate would loop back to /login.html forever
        // because isAuthenticated() can never see the session cookie.
        server->collectHeaders(F("Cookie"));
        /* if you want a simple auth you can do something like this for every page you want to "protect" */
        // server->on(F("/"), []() {
        //     if (!server->authenticate("user", "pswd")) {
        //         return server->requestAuthentication();
        //     }
        //     handleNotFound();
        // });
        server->on(F("/getconfig/"), handleGetConfig);
        server->on(F("/setconfig/"), handleSetConfig);
        server->on(F("/getcommands/"), handleGetCommandQueue);
        server->on(F("/addcommand/"), handleAddCommand);
        server->on(F("/editcommand/"), handleEditCommand);
        server->on(F("/delcommand/"), handleDelCommand);
        server->on(F("/getwebconfig/"), handleGetWebConfig);
        server->on(F("/setwebconfig/"), handleSetWebConfig);
        server->on(F("/getwifi/"), handleGetWifi);
        server->on(F("/setwifi/"), handleSetWifi);
        server->on(F("/resetwifi/"), handleResetWifi);
        server->on(F("/scanwifi/"), handleScanWifi);
        server->on(F("/logs/"), handleLogs);
        // Auth — see Code/src/webauth.{h,cpp}. Cookie-based session,
        // single password (default "spa"), site-wide lockout after 5
        // failed attempts. Every other route in this block calls
        // webauth::requireAuth() at the top of its handler.
        server->on(F("/login"),  HTTP_POST, webauth::handleLoginPost);
        server->on(F("/logout"), HTTP_POST, webauth::handleLogout);
        server->on(F("/getmqtt/"), handleGetMqtt);
        server->on(F("/setmqtt/"), handleSetMqtt);
        server->on(F("/dir/"), handleDir);
        server->on(F("/hwtest/"), handleHWtest);
        server->on(F("/inputs/"), handleInputs);
        server->on(F("/upload.html"), HTTP_POST, [](){
            server->send(200, F("text/plain"), "");
        }, handleFileUpload);
        server->on(F("/remove.html"), HTTP_POST, handleFileRemove);
        server->on(F("/remove/"), HTTP_GET, handleFileRemove);
        server->on(F("/restart/"), handleRestart);
        server->on(F("/metrics"), handlePrometheusMetrics);  //prometheus metrics
        server->on(F("/info/"), handleESPInfo);
        server->on(F("/api/watchdog-status"), HTTP_POST, handleWatchdogStatus);
        server->on(F("/sethardware/"), handleSetHardware);
        server->on(F("/gethardware/"), handleGetHardware);
        server->on(F("/debug-on/"), [](){bwc->BWC_DEBUG = true; server->send(200, F("text/plain"), "ok");});
        server->on(F("/debug-off/"), [](){bwc->BWC_DEBUG = false; server->send(200, F("text/plain"), "ok");});
        server->on(F("/cmdq_file/"), handle_cmdq_file);

        // if someone requests any other file or page, go to function 'handleNotFound'
        // and check if the file exists
        server->onNotFound(handleNotFound);
        // start the HTTP server
        server->begin();
    }
    
    // Serial.println(F("HTTP > server started"));
}

void handleGetHardware()
{
    if (!checkHttpPost(server->method())) return;
    File file = LittleFS.open(F("hwcfg.json"), "r");
    if (!file)
    {
        // Serial.println(F("Failed to open hwcfg.json"));
        server->send(404, F("text/plain"), F("not found"));
        return;
    }
    server->send(200, F("text/plain"), file.readString());
    file.close();
    BWC_YIELD;
}

void handleSetHardware()
{
    if (!checkHttpPost(server->method())) return;
    String message = server->arg(0);
    File file = LittleFS.open(F("hwcfg.json"), "w");
    if (!file)
    {
        // Serial.println(F("Failed to save hwcfg.json"));
        return;
    }
    file.print(message);
    file.close();
    server->send(200, F("text/plain"), "ok");
    BWC_YIELD;
}

void preparefortest()
{
    for(int i = 0; i < 7; i++)
    {
        pinMode(bwc->pins[i], INPUT);
    }
}

void handleInputs()
{
    if (!webauth::requireAuth()) return;
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send(200, F("text/plain"), "wait<br>");

    bwc->stop();
    preparefortest();

    /* 
        Log all edges to a file in HEAP RAM. When that log is full send to web client
    */

    unsigned long pin_states = 0, old_pin_states = 0; //to store result from READ_PERI_REG (GPIOs)
    unsigned long t; //timestamp - micros
    uint32_t edge_count = 0;
    const int array_len = 1024;
    unsigned long* p_input_log = new unsigned long[array_len*2];

    while(edge_count < array_len)
    {
        pin_states = READ_PERI_REG(PIN_IN); //mix unsigned long with uint32_t which is the same
        if(pin_states != old_pin_states)
        {
            t = micros();
            p_input_log[edge_count] = t; //log time
            p_input_log[edge_count + array_len] = pin_states; //log states (all gpios)
            edge_count++;
        }
        old_pin_states = pin_states;
        yield(); //keep the watchdog away and manage wifi etc. Unclear how much time we waste here...
    }

    /* send statistics to client */
    char s[128];
    sprintf_P(s, PSTR("micros, gpio registers\n"));
    server->sendContent(s);
    for(int i = 0; i < array_len; i++)
    {
        sprintf_P(s, PSTR("%u,%X\n"), p_input_log[i], p_input_log[i+array_len]);
        server->sendContent(s);
        yield(); //keep the watchdog away and manage wifi etc. Unclear how much time we waste here...
    }
    sprintf_P(s, PSTR("Cut and paste all above. Zip and post on forum for help.\n"));
    server->sendContent(s);
    server->sendContent("");

    delete [] p_input_log;

    bwc->setup();
}

void handleHWtest()
{
    if (!webauth::requireAuth()) return;
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send(200, F("text/plain"), "");

    int errors = 0;
    bool state = false;
    char result[128];

    bwc->stop();
    preparefortest();

    for(int i = 0; i < 10; i++)
    {
        sprintf_P(result, PSTR("\nConnect the cables now!\nStarting test in %d seconds...\n"), 10-i);
        server->sendContent(result);
        for(int t = 0; t < 512; t++)
            server->sendContent(" ");
        delay(1000);
    }

    /* First test CIO out/ DSP in ports */
    sprintf_P(result, PSTR("Start test. Seq begins with HIGH, then alters.\n\n"));
    server->sendContent(result);
    for(int pin = 0; pin < 3; pin++)
    {
        sprintf_P(result, PSTR("Sending on D%d, receiving on D%d\n"), gpio2dp(bwc->pins[pin]), gpio2dp(bwc->pins[pin+3]));
        server->sendContent(result);
        pinMode(bwc->pins[pin], OUTPUT);
        pinMode(bwc->pins[pin+3], INPUT);
        for(int t = 0; t < 100; t++)
        {
            state = !state;
            digitalWrite(bwc->pins[pin], state);
            delayMicroseconds(100);
            bool error = digitalRead(bwc->pins[pin+3]) != state;
            errors += error;
            if(error)
                if(state)
                    server->sendContent("1");
                else
                    server->sendContent("0");
            else
                server->sendContent("-");
        }
        sprintf_P(result, PSTR(" // %d errors out of 100\n"), errors);
        server->sendContent(result);
        errors = 0;
        delay(0);
    }

    /* Test the other way around */

    for(int pin = 0; pin < 3; pin++)
    {
        sprintf_P(result, PSTR("Sending on D%d, receiving on D%d\n"), gpio2dp(bwc->pins[pin+3]), gpio2dp(bwc->pins[pin]));
        server->sendContent(result);
        pinMode(bwc->pins[pin+3], OUTPUT);
        pinMode(bwc->pins[pin], INPUT);
        for(int t = 0; t < 100; t++)
        {
            state = !state;
            digitalWrite(bwc->pins[pin+3], state);
            delayMicroseconds(100);
            bool error = digitalRead(bwc->pins[pin]) != state;
            errors += error;
            if(error)
                if(state)
                    server->sendContent("1");
                else
                    server->sendContent("0");
            else
                server->sendContent("-");
        }
        sprintf_P(result, PSTR(" // %d errors out of 100\n"), errors);
        server->sendContent(result);
        errors = 0;
        delay(0);
    }

    sprintf_P(result, PSTR("End of test!\n\"1\" or \"0\" indicates ERROR, depending on test state. \"-\" is good.\n"));
    server->sendContent(result);
    sprintf_P(result, PSTR("Switching cio pins 5s HIGH -> 5s LOW -> input\n"));
    server->sendContent(result);
    sprintf_P(result, PSTR("then DSP pins 5s HIGH -> 5s LOW -> input, repeating\n"));
    server->sendContent(result);
    sprintf_P(result, PSTR("Disconnect cables then reset chip when done!\n"));
    server->sendContent(result);

    server->sendContent("");
    while(true)
    {
        /*CIO pins HIGH*/
        for(int pin = 0; pin < 3; pin++)
        {
            pinMode(bwc->pins[pin+3], INPUT);
            pinMode(bwc->pins[pin+0], OUTPUT);
            digitalWrite(bwc->pins[pin], HIGH);
        }
        delay(5000);
        /*CIO pins LOW*/
        for(int pin = 0; pin < 3; pin++)
        {
            pinMode(bwc->pins[pin+3], INPUT);
            pinMode(bwc->pins[pin+0], OUTPUT);
            digitalWrite(bwc->pins[pin], LOW);
        }
        delay(5000);
        /*DSP pins HIGH*/
        for(int pin = 0; pin < 3; pin++)
        {
            pinMode(bwc->pins[pin+0], INPUT);
            pinMode(bwc->pins[pin+3], OUTPUT);
            digitalWrite(bwc->pins[pin+3], HIGH);
        }
        delay(5000);
        /*DSP pins LOW*/
        for(int pin = 0; pin < 3; pin++)
        {
            pinMode(bwc->pins[pin+0], INPUT);
            pinMode(bwc->pins[pin+3], OUTPUT);
            digitalWrite(bwc->pins[pin+3], LOW);
        }
        delay(5000);
    }
    bwc->setup();
}

void handleNotFound()
{
    // check if the file exists in the flash memory (LittleFS), if so, send it
    if (!handleFileRead(server->uri()))
    {
        server->send(404, F("text/plain"), F("404: File Not Found"));
    }
}

String getContentType(const String& filename)
{
    if (filename.endsWith(".html")) return F("text/html");
    else if (filename.endsWith(".css")) return F("text/css");
    else if (filename.endsWith(".js")) return F("application/javascript");
    else if (filename.endsWith(".ico")) return F("image/x-icon");
    else if (filename.endsWith(".gz")) return F("application/x-gzip");
    else if (filename.endsWith(".json")) return F("application/json");
    return F("text/plain");
}

/**
 * send the right file to the client (if it exists)
 */
bool handleFileRead(String path)
{
    pause_all(true);
    // Serial.println("HTTP > request: " + path);
    // If a folder is requested, send the index file
    if (path.endsWith("/"))
    {
        path += F("index.html");
    }
    // deny reading credentials
    if (path.equalsIgnoreCase("/mqtt.json") || path.equalsIgnoreCase("/wifi.json") ||
        path.equalsIgnoreCase("/auth.json"))
    {
        server->send(403, F("text/plain"), F("Permission denied."));
        // Serial.println(F("HTTP > file reading denied (credentials)."));
        pause_all(false);
        return false;
    }
    // Auth gate — only on HTML page requests. Static assets (.css, .js,
    // .png, .svg, .ico, fonts) pass through without auth so the login
    // page itself can load its CSS/JS. The auth-routes (login.html
    // itself, /login, /logout) are also exempt — see isAuthRouteUri.
    // Sensitive *.json files are already 403'd above so we don't gate
    // those here either.
    bool isHtml = path.endsWith(F(".html")) || path.endsWith(F(".html.gz"));
    if (isHtml && !webauth::isAuthRouteUri(path) && !webauth::requireAuth())
    {
        // requireAuth() already sent the redirect to /login.html.
        // Return true so the caller (handleNotFound) doesn't also
        // send a 404 on top.
        pause_all(false);
        return true;
    }
    String contentType = getContentType(path);                  // Get the MIME type
    String pathWithGz = path + ".gz";
    if (LittleFS.exists(pathWithGz) || LittleFS.exists(path)) { // If the file exists, either as a compressed archive, or normal
        if (LittleFS.exists(pathWithGz))                        // If there's a compressed version available
            path += ".gz";                                      // Use the compressed version
        File file = LittleFS.open(path, "r");                   // Open the file
        size_t fsize = file.size();
        BWC_YIELD;
        size_t sent = server->streamFile(file, contentType);    // Send it to the client
        BWC_LOG_P(PSTR("File size: %d\n"),fsize);
        BWC_LOG_P(PSTR("HTTPServer > Filename: %s. Bytes sent: %d\n"),path.c_str(),sent);
        if(fsize != sent){
            BWC_LOG_P(PSTR("^^^^^ File not completed ^^^^^\n"),0);
        }
        pause_all(false);
        file.close();                                           // Close the file again
        return true;
    }
    pause_all(false);
    // If the file doesn't exist, return false
    return false;
}

/**
 * checks the method to be a POST
 */
bool checkHttpPost(HTTPMethod method)
{
    // Auth gate FIRST — never leak a "405 Method not allowed" response
    // to an unauthenticated client (gives away that an endpoint exists).
    // requireAuth() sends the 401 or 302 itself on failure; we just need
    // to bail out of the handler.
    if (!webauth::requireAuth()) return false;
    if (method != HTTP_POST)
    {
        server->send(405, F("text/plain"), F("Method not allowed."));
        return false;
    }
    return true;
}

/**
 * response for /getconfig/
 * web server prints a json document
 */
void handleGetConfig()
{
    if (!checkHttpPost(server->method())) return;

    String json;
    json.reserve(320);
    bwc->getJSONSettings(json);
    server->send(200, F("text/plain"), json);
    BWC_YIELD;
}

/**
 * response for /setconfig/
 * web server writes a json document
 */
void handleSetConfig()
{
    if (!checkHttpPost(server->method())) return;

    String message = server->arg(0);
    bwc->setJSONSettings(message);

    server->send(200, F("text/plain"), "");
    send_mqtt_cfg_needed = true;
    BWC_YIELD;
}

/**
 * response for /getcommands/
 * web server prints a json document
 */
void handleGetCommandQueue()
{
    if (!checkHttpPost(server->method())) return;

    String json = bwc->getJSONCommandQueue();
    server->send(200, F("application/json"), json);
}

/**
 * response for /addcommand/
 * add a command to the queue
 */
void handleAddCommand()
{
    // if (!checkHttpPost(server->method())) return;

    // DynamicJsonDocument doc(256);
    StaticJsonDocument<256> doc;
    String message = server->arg(0);
    DeserializationError error = deserializeJson(doc, message);
    if (error)
    {
        server->send(400, F("text/plain"), F("Error deserializing message: ")+message);
        return;
    }

    Commands command = doc[F("CMD")];
    int64_t value = doc[F("VALUE")];
    int64_t xtime = doc[F("XTIME")];
    int64_t interval = doc[F("INTERVAL")];
    String txt = doc[F("TXT")] | "";
    command_que_item item;
    item.cmd = command;
    item.val = value;
    item.xtime = xtime;
    item.interval = interval;
    item.text = txt;
    bwc->add_command(item);

    server->send(200, F("text/plain"), F("ok"));
}

/**
 * response for /editcommand/
 * replace a command in the queue with new command
 */
void handleEditCommand()
{
    if (!checkHttpPost(server->method())) return;

    // DynamicJsonDocument doc(256);
    StaticJsonDocument<256> doc;
    String message = server->arg(0);
    DeserializationError error = deserializeJson(doc, message);
    if (error)
    {
        server->send(400, F("text/plain"), F("Error deserializing message"));
        return;
    }

    Commands command = doc[F("CMD")];
    int64_t value = doc[F("VALUE")];
    int64_t xtime = doc[F("XTIME")];
    int64_t interval = doc[F("INTERVAL")];
    String txt = doc[F("TXT")] | "";
    uint8_t index = doc[F("IDX")];
    command_que_item item;
    item.cmd = command;
    item.val = value;
    item.xtime = xtime;
    item.interval = interval;
    item.text = txt;
    bwc->edit_command(index, item);

    server->send(200, F("text/plain"), "");
}

/**
 * response for /delcommand/
 * replace a command in the queue with new command
 */
void handleDelCommand()
{
    if (!checkHttpPost(server->method())) return;

    // DynamicJsonDocument doc(256);
    StaticJsonDocument<256> doc;
    String message = server->arg(0);
    DeserializationError error = deserializeJson(doc, message);
    if (error)
    {
        server->send(400, F("text/plain"), F("Error deserializing message"));
        return;
    }

    uint8_t index = doc[F("IDX")];
    bwc->del_command(index);
    server->send(200, F("text/plain"), "");
}

void handle_cmdq_file()
{
    if (!checkHttpPost(server->method())) return;

    // DynamicJsonDocument doc(256);
    StaticJsonDocument<256> doc;
    String message = server->arg(0);
    DeserializationError error = deserializeJson(doc, message);
    if (error)
    {
        server->send(400, F("text/plain"), F("Error deserializing message"));
        return;
    }

    String action = doc[F("ACT")].as<String>();
    String filename = "/";
    filename += doc[F("NAME")].as<String>();

    if(action.equals("load"))
    {
        copyFile("/cmdq.json", "/cmdq.backup");
        copyFile(filename, "/cmdq.json");
        bwc->reloadCommandQueue();
    }
    if(action.equals("save"))
    {
        copyFile("/cmdq.json", filename);
    }

    server->send(200, F("text/plain"), "");
    BWC_YIELD;
}

void copyFile(String source, String dest)
{
    char ibuffer[64];  //declare a buffer
    
    File f_source = LittleFS.open(source, "r");    //open source file to read
    if (!f_source)
    {
        return;
    }

    File f_dest = LittleFS.open(dest, "w");    //open destination file to write
    if (!f_dest)
    {
        return;
    }
    
    while (f_source.available() > 0)
    {
        byte i = f_source.readBytes(ibuffer, 64); // i = number of bytes placed in buffer from file f_source
        f_dest.write(ibuffer, i);               // write i bytes from buffer to file f_dest
    }
    
    f_dest.close(); // done, close the destination file
    f_source.close(); // done, close the source file
    BWC_YIELD;
}

/**
 * load "Web Config" json configuration from "webconfig.json"
 */
void loadWebConfig()
{
    // DynamicJsonDocument doc(1024);
    StaticJsonDocument<256> doc;

    File file = LittleFS.open(F("/webconfig.json"), "r");
    if (file)
    {
        DeserializationError error = deserializeJson(doc, file);
        if (error)
        {
        // Serial.println(F("Failed to deserialize webconfig.json"));
        file.close();
        return;
        }
    }
    else
    {
        // Serial.println(F("Failed to read webconfig.json. Using defaults."));
    }

    showSectionTemperature = (doc.containsKey(F("SST")) ? doc[F("SST")] : true);
    showSectionDisplay = (doc.containsKey(F("SSD")) ? doc[F("SSD")] : true);
    showSectionControl = (doc.containsKey(F("SSC")) ? doc[F("SSC")] : true);
    showSectionButtons = (doc.containsKey(F("SSB")) ? doc[F("SSB")] : true);
    showSectionTimer = (doc.containsKey(F("SSTIM")) ? doc[F("SSTIM")] : true);
    showSectionTotals = (doc.containsKey(F("SSTOT")) ? doc[F("SSTOT")] : true);
    useControlSelector = (doc.containsKey(F("UCS")) ? doc[F("UCS")] : false);
    BWC_YIELD;
}

/**
 * save "Web Config" json configuration to "webconfig.json"
 */
void saveWebConfig()
{
    File file = LittleFS.open(F("/webconfig.json"), "w");
    if (!file)
    {
        // Serial.println(F("Failed to save webconfig.json"));
        return;
    }

    // DynamicJsonDocument doc(256);
    StaticJsonDocument<256> doc;

    doc[F("SST")] = showSectionTemperature;
    doc[F("SSD")] = showSectionDisplay;
    doc[F("SSC")] = showSectionControl;
    doc[F("SSB")] = showSectionButtons;
    doc[F("SSTIM")] = showSectionTimer;
    doc[F("SSTOT")] = showSectionTotals;
    doc[F("UCS")] = useControlSelector;

    if (serializeJson(doc, file) == 0)
    {
        // Serial.println(F("{\"error\": \"Failed to serialize file\"}"));
    }
    file.close();
    BWC_YIELD;
}

/**
 * response for /getwebconfig/
 * web server prints a json document
 */
void handleGetWebConfig()
{
    if (!checkHttpPost(server->method())) return;

    // DynamicJsonDocument doc(256);
    StaticJsonDocument<256> doc;

    doc[F("SST")] = showSectionTemperature;
    doc[F("SSD")] = showSectionDisplay;
    doc[F("SSC")] = showSectionControl;
    doc[F("SSB")] = showSectionButtons;
    doc[F("SSTIM")] = showSectionTimer;
    doc[F("SSTOT")] = showSectionTotals;
    doc[F("UCS")] = useControlSelector;

    String json;
    if (serializeJson(doc, json) == 0)
    {
        json = F("{\"error\": \"Failed to serialize webcfg\"}");
    }
    server->send(200, F("application/json"), json);
}

/**
 * response for /setwebconfig/
 * web server writes a json document
 */
void handleSetWebConfig()
{
    if (!checkHttpPost(server->method())) return;

    // DynamicJsonDocument doc(256);
    StaticJsonDocument<256> doc;
    String message = server->arg(0);
    DeserializationError error = deserializeJson(doc, message);
    if (error)
    {
        // Serial.println(F("Failed to read config file"));
        server->send(400, F("text/plain"), F("Error deserializing message"));
        return;
    }

    showSectionTemperature = doc[F("SST")];
    showSectionDisplay = doc[F("SSD")];
    showSectionControl = doc[F("SSC")];
    showSectionButtons = doc[F("SSB")];
    showSectionTimer = doc[F("SSTIM")];
    showSectionTotals = doc[F("SSTOT")];
    useControlSelector = doc[F("UCS")];

    saveWebConfig();

    server->send(200, F("text/plain"), "");
}

/**
 * load WiFi json configuration from "wifi.json"
 */
void loadWifi()
{
    File file = LittleFS.open(F("/wifi.json"), "r");
    if (!file)
    {
        // Serial.println(F("Failed to read wifi.json. Using defaults."));
        return;
    }

    DynamicJsonDocument doc(1024);

    DeserializationError error = deserializeJson(doc, file);
    if (error)
    {
        // Serial.println(F("Failed to deserialize wifi.json"));
        file.close();
        return;
    }

    wifi_info->enableAp = doc[F("enableAp")];
    if(doc.containsKey(F("enableWM"))) wifi_info->enableWmApFallback = doc[F("enableWM")];
    wifi_info->apSsid = doc[F("apSsid")].as<String>();
    wifi_info->apPwd = doc[F("apPwd")].as<String>();
    // apBssid added 2026-06-01 — backwards-compatible with wifi.json files
    // written by older firmwares (missing key → empty string → no pin).
    if (doc.containsKey(F("apBssid"))) wifi_info->apBssid = doc[F("apBssid")].as<String>();

    wifi_info->enableStaticIp4 = doc[F("enableStaticIp4")];
    String s(30);
    wifi_info->ip4Address_str = doc[F("ip4Address")].as<String>();
    wifi_info->ip4Gateway_str = doc[F("ip4Gateway")].as<String>();
    wifi_info->ip4Subnet_str = doc[F("ip4Subnet")].as<String>();
    wifi_info->ip4DnsPrimary_str = doc[F("ip4DnsPrimary")].as<String>();
    wifi_info->ip4DnsSecondary_str = doc[F("ip4DnsSecondary")].as<String>();
    wifi_info->ip4NTP_str = doc[F("ip4NTP")].as<String>();

    BWC_YIELD;
}

/**
 * save WiFi json configuration to "wifi.json"
 */
void saveWifi()
{
    File file = LittleFS.open(F("/wifi.json"), "w");
    if (!file)
    {
        // Serial.println(F("Failed to save wifi.json"));
        return;
    }

    DynamicJsonDocument doc(1024);

    doc[F("enableAp")] = wifi_info->enableAp;
    doc[F("enableWM")] = wifi_info->enableWmApFallback;
    doc[F("apSsid")] = wifi_info->apSsid;
    doc[F("apPwd")] = wifi_info->apPwd;
    doc[F("apBssid")] = wifi_info->apBssid;
    doc[F("enableStaticIp4")] = wifi_info->enableStaticIp4;
    doc[F("ip4Address")] = wifi_info->ip4Address_str;
    doc[F("ip4Gateway")] = wifi_info->ip4Gateway_str;
    doc[F("ip4Subnet")] = wifi_info->ip4Subnet_str;
    doc[F("ip4DnsPrimary")] = wifi_info->ip4DnsPrimary_str;
    doc[F("ip4DnsSecondary")] = wifi_info->ip4DnsSecondary_str;
    doc[F("ip4NTP")] = wifi_info->ip4NTP_str;

    if (serializeJson(doc, file) == 0)
    {
        // Serial.println(F("{\"error\": \"Failed to serialize file\"}"));
    }
    file.close();
    BWC_YIELD;
}

/**
 * response for /getwifi/
 * web server prints a json document
 */
void handleGetWifi()
{
    if (!checkHttpPost(server->method())) return;

    DynamicJsonDocument doc(1024);

    doc[F("enableAp")] = wifi_info->enableAp;
    doc[F("enableWM")] = wifi_info->enableWmApFallback;
    doc[F("apSsid")] = wifi_info->apSsid;
    doc[F("apBssid")] = wifi_info->apBssid;
    doc[F("apPwd")] = F("<enter password>");
    if (!hidePasswords)
    {
        doc[F("apPwd")] = wifi_info->apPwd;
    }

    // Live connection status — surfaced to the UI so users can confirm
    // the device actually joined their home WiFi after Save+Restart,
    // and so we can diagnose "thought I connected but it didn't" cases
    // without needing serial access (which on this adapter is wired to
    // the spa's CIO/DSP, not USB). See also ADR-007-style note in the
    // README of the fork.
    JsonObject st = doc.createNestedObject(F("status"));
    wl_status_t wlst = WiFi.status();
    st[F("wifiStatus")]    = (int)wlst;
    st[F("connected")]     = wlst == WL_CONNECTED;
    st[F("mode")]          = (WiFi.getMode() == WIFI_AP)     ? "AP"
                           : (WiFi.getMode() == WIFI_STA)    ? "STA"
                           : (WiFi.getMode() == WIFI_AP_STA) ? "AP+STA"
                           : "OFF";
    if (wlst == WL_CONNECTED) {
        st[F("ssid")]      = WiFi.SSID();
        st[F("bssid")]     = WiFi.BSSIDstr();
        st[F("rssi")]      = WiFi.RSSI();
        st[F("channel")]   = WiFi.channel();
        st[F("ip")]        = WiFi.localIP().toString();
        st[F("gateway")]   = WiFi.gatewayIP().toString();
        st[F("subnet")]    = WiFi.subnetMask().toString();
        st[F("dns")]       = WiFi.dnsIP().toString();
        st[F("mac")]       = WiFi.macAddress();
    }
    // Always include softAP info — useful when the user is HITTING this
    // endpoint via the softAP and wants to confirm it's actually live.
    if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
        st[F("apIp")]      = WiFi.softAPIP().toString();
        st[F("apClients")] = WiFi.softAPgetStationNum();
    }

    doc[F("enableStaticIp4")] = wifi_info->enableStaticIp4;
    doc[F("ip4Address")] = wifi_info->ip4Address_str;
    doc[F("ip4Gateway")] = wifi_info->ip4Gateway_str;
    doc[F("ip4Subnet")] = wifi_info->ip4Subnet_str;
    doc[F("ip4DnsPrimary")] = wifi_info->ip4DnsPrimary_str;
    doc[F("ip4DnsSecondary")] = wifi_info->ip4DnsSecondary_str;
    doc[F("ip4NTP")] = wifi_info->ip4NTP_str;
    String json;
    json.reserve(200);
    if (serializeJson(doc, json) == 0)
    {
        json = F("{\"error\": \"Failed to serialize message\"}");
    }
    server->send(200, F("application/json"), json);
}

/**
 * response for /setwifi/
 * web server writes a json document
 */
void handleSetWifi()
{
    if (!checkHttpPost(server->method())) return;

    DynamicJsonDocument doc(1024);
    String message = server->arg(0);
    DeserializationError error = deserializeJson(doc, message);
    if (error)
    {
        // Serial.println(F("Failed to read config file"));
        server->send(400, F("text/plain"), F("Error deserializing message"));
        return;
    }

    wifi_info->enableAp = doc[F("enableAp")];
    if(doc.containsKey("enableWM")) wifi_info->enableWmApFallback = doc[F("enableWM")];
    wifi_info->apSsid = doc[F("apSsid")].as<String>();
    wifi_info->apPwd = doc[F("apPwd")].as<String>();
    if (doc.containsKey(F("apBssid"))) wifi_info->apBssid = doc[F("apBssid")].as<String>();

    wifi_info->enableStaticIp4 = doc[F("enableStaticIp4")];
    wifi_info->ip4Address_str = doc[F("ip4Address")].as<String>();
    wifi_info->ip4Gateway_str = doc[F("ip4Gateway")].as<String>();
    wifi_info->ip4Subnet_str = doc[F("ip4Subnet")].as<String>();
    wifi_info->ip4DnsPrimary_str = doc[F("ip4DnsPrimary")].as<String>();
    wifi_info->ip4DnsSecondary_str = doc[F("ip4DnsSecondary")].as<String>();
    wifi_info->ip4NTP_str = doc[F("ip4NTP")].as<String>();

    saveWifi();

    server->send(200, F("text/plain"), "");
}

/**
 * response for /scanwifi/
 * Runs a synchronous WiFi scan and returns a JSON array of nearby APs.
 * Each entry: { ssid, bssid, rssi, channel, secure }.
 *
 *   • Sorted by RSSI descending (strongest signal first).
 *   • secure = true for any encryption mode other than OPEN.
 *   • Duplicate SSIDs are preserved (that's exactly what mesh users need
 *     to see — multiple BSSIDs sharing one SSID, with different RSSI).
 *
 * Synchronous scan blocks the main loop for ~2 s. Acceptable here because
 * the user explicitly clicks "Scan", but don't auto-invoke this from any
 * periodic task — it'd freeze the spa command queue while running.
 */
void handleScanWifi()
{
    if (!webauth::requireAuth()) return;
    DynamicJsonDocument doc(2048);
    JsonArray nets = doc.createNestedArray(F("networks"));

    // ESP8266 scanning gotchas:
    //   • pure AP mode can't scan — need STA or AP+STA. The user is
    //     hitting this via the softAP, so the device IS in AP+STA, but
    //     set it explicitly in case some earlier code path flipped it.
    //   • a leftover scan result from a previous attempt makes the next
    //     call return -1 (WIFI_SCAN_RUNNING) forever. scanDelete() clears
    //     that state.
    //   • when the STA isn't connected, the scan still works — the
    //     hardware doesn't need an association to listen for beacons.
    WiFi.mode(WIFI_AP_STA);
    WiFi.scanDelete();
    BWC_YIELD;

    int n = WiFi.scanNetworks(false /* async */, true /* show hidden */);
    if (n < 0) {
        // Surface the real error code so we can tell -1 (running) from
        // -2 (failed). Also include the free-heap so we can diagnose
        // the OOM case if it ever crops up — scan allocates ~5 KB transiently.
        char errBuf[96];
        snprintf(errBuf, sizeof(errBuf),
                 "WiFi.scanNetworks() returned %d (heap=%u)",
                 n, (unsigned)ESP.getFreeHeap());
        doc[F("error")] = errBuf;
        String json;
        serializeJson(doc, json);
        server->send(500, F("application/json"), json);
        return;
    }

    // Collect indices and sort by RSSI descending. Avoids ArduinoJson's
    // shuffle quirks if we tried to sort the array post-fill.
    int order[32];
    int kept = (n < 32) ? n : 32;
    for (int i = 0; i < kept; i++) order[i] = i;
    for (int i = 0; i < kept - 1; i++) {
        for (int j = i + 1; j < kept; j++) {
            if (WiFi.RSSI(order[j]) > WiFi.RSSI(order[i])) {
                int t = order[i]; order[i] = order[j]; order[j] = t;
            }
        }
    }

    for (int k = 0; k < kept; k++) {
        int i = order[k];
        JsonObject net = nets.createNestedObject();
        net[F("ssid")]    = WiFi.SSID(i);
        net[F("bssid")]   = WiFi.BSSIDstr(i);
        net[F("rssi")]    = WiFi.RSSI(i);
        net[F("channel")] = WiFi.channel(i);
        #ifdef ESP8266
        net[F("secure")]  = WiFi.encryptionType(i) != ENC_TYPE_NONE;
        #else
        net[F("secure")]  = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        #endif
    }

    WiFi.scanDelete();   // Free the scan-result buffer in the WiFi driver.

    String json;
    json.reserve(256 + kept * 96);
    serializeJson(doc, json);
    server->send(200, F("application/json"), json);
}

/**
 * response for /logs/
 * Returns the full in-memory log ring buffer as plain text. Easy to
 * copy from the browser and paste back into a chat / issue for
 * debugging. Substitutes for USB serial debug on this hardware.
 */
void handleLogs()
{
    if (!webauth::requireAuth()) return;
    String body = logbuf::dump();
    server->sendHeader(F("Cache-Control"), F("no-store"));
    server->send(200, F("text/plain; charset=utf-8"), body);
}

/*
 * response for /resetwifi/
 * do this before giving away the device (be aware of other credentials e.g. MQTT)
 * a complete flash erase should do the job but remember to upload the filesystem as well.
 */
void handleResetWifi()
{
    if (!webauth::requireAuth()) return;
    server->send(200, F("text/html"), F("WiFi connection reset (erase) ..."));
    // Serial.println(F("WiFi connection reset (erase) ..."));
    resetWiFi();

    server->send(200, F("text/html"), F("WiFi connection reset (erase) ... done."));
    // Serial.println(F("WiFi connection reset (erase) ... done."));
    // Serial.println(F("ESP reset ..."));
    #if defined(ESP8266)
    ESP.reset();
    #else
    ESP.restart();
    #endif
}

void resetWiFi()
{
    wifi_info->enableAp = false;
    wifi_info->enableWmApFallback = true;
    wifi_info->apSsid = F("empty");
    wifi_info->apPwd = F("empty");
    saveWifi();
    delay(3000);
    periodicTimer->detach();
    updateMqttTimer->detach();
    updateWSTimer->detach();
    if(ntpCheck_ticker->active()) ntpCheck_ticker->detach();
    bwc->saveSettings();
    bwc->stop();
    delay(1000);
#if defined(ESP8266)
    ESP.eraseConfig();
#endif
    delay(1000);
    // ESP_WiFiManager wm;
    // wm.resetSettings();
    //WiFi.disconnect();
    delay(1000);
}

/**
 * load MQTT json configuration from "mqtt.json"
 */
void loadMqtt()
{
    File file = LittleFS.open("mqtt.json", "r");
    if (!file)
    {
        BWC_LOG_P(PSTR("MQTT > Failed to read mqtt.json. Using defaults.\n"),0);
        return;
    }

    DynamicJsonDocument doc(1024);

    DeserializationError error = deserializeJson(doc, file);
    if (error)
    {
        // Serial.println(F("Failed to deserialize mqtt.json."));
        file.close();
        return;
    }

    mqtt_info->useMqtt = doc[F("enableMqtt")];
    mqtt_info->mqttHost = doc[F("mqttHost")].as<String>();
    mqtt_info->mqttPort = doc[F("mqttPort")];
    mqtt_info->mqttUsername = doc[F("mqttUsername")].as<String>();
    mqtt_info->mqttPassword = doc[F("mqttPassword")].as<String>();
    mqtt_info->mqttClientId = doc[F("mqttClientId")].as<String>();
    mqtt_info->mqttBaseTopic = doc[F("mqttBaseTopic")].as<String>();
    mqtt_info->mqttTelemetryInterval = doc[F("mqttTelemetryInterval")];
    // haTempUnit added 2026-06-02. Older mqtt.json files won't have it —
    // .as<String>() returns "null" which we coerce to "C" so existing
    // installs default sensibly without needing a re-save.
    {
      String hu = doc[F("haTempUnit")].as<String>();
      mqtt_info->haTempUnit = (hu == "F") ? "F" : "C";
    }
    BWC_YIELD;
}

/**
 * save MQTT json configuration to "mqtt.json"
 */
void saveMqtt()
{
    File file = LittleFS.open("mqtt.json", "w");
    if (!file)
    {
        // Serial.println(F("Failed to save mqtt.json"));
        return;
    }

    DynamicJsonDocument doc(1024);

    doc[F("enableMqtt")] = mqtt_info->useMqtt;
    doc[F("mqttHost")] = mqtt_info->mqttHost;
    doc[F("mqttPort")] = mqtt_info->mqttPort;
    doc[F("mqttUsername")] = mqtt_info->mqttUsername;
    doc[F("mqttPassword")] = mqtt_info->mqttPassword;
    doc[F("mqttClientId")] = mqtt_info->mqttClientId;
    doc[F("mqttBaseTopic")] = mqtt_info->mqttBaseTopic;
    doc[F("mqttTelemetryInterval")] = mqtt_info->mqttTelemetryInterval;
    doc[F("haTempUnit")] = mqtt_info->haTempUnit;

    if (serializeJson(doc, file) == 0)
    {
        // Serial.println(F("{\"error\": \"Failed to serialize file\"}"));
    }
    file.close();
    BWC_YIELD;
}

/**
 * response for /getmqtt/
 * web server prints a json document
 */
void handleGetMqtt()
{
    if (!checkHttpPost(server->method())) return;

    DynamicJsonDocument doc(1024);

    doc[F("enableMqtt")] = mqtt_info->useMqtt;
    doc[F("mqttHost")] = mqtt_info->mqttHost;
    doc[F("mqttPort")] = mqtt_info->mqttPort;
    doc[F("mqttUsername")] = mqtt_info->mqttUsername;
    doc[F("mqttPassword")] = "<enter password>";
    if (!hidePasswords)
    {
        doc[F("mqttPassword")] = mqtt_info->mqttPassword;
    }
    doc[F("mqttClientId")] = mqtt_info->mqttClientId;
    doc[F("mqttBaseTopic")] = mqtt_info->mqttBaseTopic;
    doc[F("mqttTelemetryInterval")] = mqtt_info->mqttTelemetryInterval;
    doc[F("haTempUnit")] = mqtt_info->haTempUnit;

    String json;
    if (serializeJson(doc, json) == 0)
    {
        json = F("{\"error\": \"Failed to serialize message\"}");
    }
    server->send(200, F("text/plain"), json);
    BWC_YIELD;
}

/**
 * response for /setmqtt/
 * web server writes a json document
 */
void handleSetMqtt()
{
    if (!checkHttpPost(server->method())) return;

    DynamicJsonDocument doc(1024);
    String message = server->arg(0);
    DeserializationError error = deserializeJson(doc, message);
    if (error)
    {
        // Serial.println(F("Failed to read config file"));
        server->send(400, F("text/plain"), F("Error deserializing message"));
        return;
    }

    mqtt_info->useMqtt = doc[F("enableMqtt")];
    mqtt_info->mqttHost = doc[F("mqttHost")].as<String>();
    mqtt_info->mqttPort = doc[F("mqttPort")];
    mqtt_info->mqttUsername = doc[F("mqttUsername")].as<String>();
    mqtt_info->mqttPassword = doc[F("mqttPassword")].as<String>();
    mqtt_info->mqttClientId = doc[F("mqttClientId")].as<String>();
    mqtt_info->mqttBaseTopic = doc[F("mqttBaseTopic")].as<String>();
    mqtt_info->mqttTelemetryInterval = doc[F("mqttTelemetryInterval")];

    server->send(200, F("text/plain"), "");

    saveMqtt();
    startMqtt();
    BWC_YIELD;
}

/**
 * response for /dir/
 * web server prints a list of files
 */
void handleDir()
{
    if (!webauth::requireAuth()) return;
    // HeapSelectIram ephemeral;
    String mydir;
    mydir.reserve(128);
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send(200, F("text/html"), "");
    Dir root = LittleFS.openDir("/");
    while (root.next())
    {
        // Serial.println(root.fileName());
        String href = root.fileName();
        if (href.endsWith(".gz")) href.remove(href.length()-3);
        mydir += F("<a href=\"/");
        mydir +=href;
        mydir += F("\">");
        mydir += root.fileName();
        mydir += F("</a>");
        mydir += F(" Size: ");
        mydir += String(root.fileSize());
        mydir += F(" Bytes ");
        mydir += F(" <a href=\"/remove/?FileToRemove=");
        mydir += root.fileName();
        mydir += F("\">remove</a><br>");
        server->sendContent(mydir);
        mydir.clear();
    }
    server->sendContent("");
}

/**
 * response for /upload.html
 * upload a new file to the LittleFS
 */
void handleFileUpload()
{
    if (!webauth::requireAuth()) return;
    HTTPUpload& upload = server->upload();
    String path;
    /** a file variable to temporarily store the received file */
    if (upload.status == UPLOAD_FILE_START)
    {
        path = upload.filename;
        if (!path.startsWith("/"))
        {
            path = "/" + path;
        }

        // The file server always prefers a compressed version of a file
        if (!path.endsWith(".gz"))
        {
            // So if an uploaded file is not compressed, the existing compressed
            String pathWithGz = path + ".gz";
            // version of that file must be deleted (if it exists)
            if (LittleFS.exists(pathWithGz))
            {
                LittleFS.remove(pathWithGz);
            }
        }

        BWC_LOG_P(PSTR("FS > upload filename: %s\n"),path.c_str());

        // Open the file for writing in LittleFS (create if it doesn't exist)
        fsUploadFile = LittleFS.open(path, "w");
        path = String();
    }
    else if (upload.status == UPLOAD_FILE_WRITE)
    {
        if (fsUploadFile)
        {
            // Write the received bytes to the file
            fsUploadFile.write(upload.buf, upload.currentSize);
            // Serial.print("file write ");
            // Serial.println(path);
        }
    }
    else if (upload.status == UPLOAD_FILE_END)
    {
        if (fsUploadFile)
        {
            fsUploadFile.close();
            BWC_LOG_P(PSTR("FS > upload size: %d\n"),upload.totalSize);
            server->sendHeader(F("location"), F("success.html"));
            server->send(303);
            if (upload.filename == "cmdq.json")
            {
                bwc->reloadCommandQueue();
            }
            if (upload.filename == "settings.json")
            {
                bwc->reloadSettings();
            }
        }
        else
        {
            BWC_LOG_P(PSTR("FA > error: %d\n"), upload.status);
            server->send(500, F("text/plain"), F("500: couldn't create file"));
        }
    }
    else
    {
        BWC_LOG_P(PSTR("FA > upload aborted: %d\n"), upload.status);
        server->send(500, F("text/plain"), F("500: upload aborted"));
    }
}

/**
 * response for /remove.html
 * delete a file from the LittleFS
 */
void handleFileRemove()
{
    if (!webauth::requireAuth()) return;
    String path;
    path = server->arg(F("FileToRemove"));
    if (!path.startsWith("/"))
    {
        path = "/" + path;
    }

    // Serial.print(F("handleFileRemove Name: "));
    // Serial.println(path);

    if (LittleFS.exists(path) && LittleFS.remove(path))
    {
        // Serial.print(F("handleFileRemove success: "));
        // Serial.println(path);
        if(server->method() == HTTP_GET)
            server->sendHeader(F("Location"), F("/dir/"));
        else
            server->sendHeader(F("Location"), F("/success.html"));
        server->send(303);
    }
    else
    {
        // Serial.print(F("handleFileRemove error: "));
        // Serial.println(path);
        server->send(500, F("text/plain"), F("500: couldn't delete file"));
    }
}

/**
 * response for /restart/
 */
void handleRestart()
{
    if (!webauth::requireAuth()) return;
    server->send(200, F("text/html"), F("ESP restart ..."));

    server->sendHeader(F("Location"), "/");
    server->send(303);

    delay(1000);
    stopall();
    delay(1000);
    BWC_LOG_P(PSTR("ESP restart ...\n"),0);
    ESP.restart();
    delay(3000);
}

void updateStart(){
    BWC_LOG_P(PSTR("OTA > update start\n"),0);
}
void updateEnd(){
    BWC_LOG_P(PSTR("OTA > update finish\n"),0);
}
void udpateProgress(int cur, int total){
    BWC_LOG_P(PSTR("OTA: update process at %d of %d bytes...\n"), cur, total);
}
void updateError(int err){
    BWC_LOG_P(PSTR("update fatal error code %d\n"), err);
}

/**
 * MQTT setup and connect
 * @author 877dev
 */
void startMqtt()
{
    {
        HeapSelectIram ephemeral;
        BWC_LOG_P(PSTR("MQTT > start. Iram heap: %d\n"), ESP.getFreeHeap());
        if(!aWifiClient) aWifiClient = new WiFiClient;
        if(!mqttClient) mqttClient = new PubSubClient(*aWifiClient);
    

        // load mqtt credential file if it exists, and update default strings
        loadMqtt();

        // disconnect in case we are already connected
        mqttClient->disconnect();

        // setup MQTT broker information as defined earlier
        mqttClient->setServer(mqtt_info->mqttHost.c_str(), mqtt_info->mqttPort);
        // set buffer for larger messages, new to library 2.8.0
        // if (mqttClient->setBufferSize(1536))
        {
            // Serial.println(F("MQTT > Buffer size successfully increased"));
        }
        mqttClient->setKeepAlive(60);
        mqttClient->setSocketTimeout(30);
        // set callback details
        // this function is called automatically whenever a message arrives on a subscribed topic.
        mqttClient->setCallback(mqttCallback);
        // Connect to MQTT broker, publish Status/MAC/count, and subscribe to keypad topic.
    }
    mqttConnect();
    BWC_YIELD;
}

/**
 * MQTT callback function
 * @author 877dev
 */
void mqttCallback(char* topic, byte* payload, unsigned int length)
{
    // Serial.print(F("MQTT > Message arrived ["));
    // Serial.print(topic);
    // Serial.print(")] ");
    for (unsigned int i = 0; i < length; i++)
    {
        // Serial.print((char)payload[i]);
    }
    // Serial.println();
    if (String(topic).equals(String(mqtt_info->mqttBaseTopic) + F("/command")))
    {
        // DynamicJsonDocument doc(256);
        StaticJsonDocument<256> doc;
        String message = (const char *) &payload[0];
        DeserializationError error = deserializeJson(doc, message);
        if (error)
        {
            return;
        }

        Commands command = doc[F("CMD")];
        int64_t value = doc[F("VALUE")];
        int64_t xtime = doc[F("XTIME")];
        int64_t interval = doc[F("INTERVAL")];
        String txt = doc[F("TXT")] | "";
        command_que_item item;
        item.cmd = command;
        item.val = value;
        item.xtime = xtime;
        item.interval = interval;
        item.text = txt;
        bwc->add_command(item);
        return;
    }

    /* author @malfurion, edited by @visualapproach for v4 */
    if (String(topic).equals(String(mqtt_info->mqttBaseTopic) + F("/command_batch")))
    {
        DynamicJsonDocument doc(1024);
        String message = (const char *) &payload[0];
        DeserializationError error = deserializeJson(doc, message);
        if (error)
        {
            return;
        }

        JsonArray commandArray = doc.as<JsonArray>();

        for (JsonVariant commandItem : commandArray) {
            Commands command = commandItem[F("CMD")];
            int64_t value = commandItem[F("VALUE")];
            int64_t xtime = commandItem[F("XTIME")];
            int64_t interval = commandItem[F("INTERVAL")];
            String txt = doc[F("TXT")] | "";
            command_que_item item;
            item.cmd = command;
            item.val = value;
            item.xtime = xtime;
            item.interval = interval;
            item.text = txt;
            bwc->add_command(item);
        }

        return;
    }

    if (String(topic).equals(String(mqtt_info->mqttBaseTopic) + F("/set_config")))
    {
        String message = (const char *) &payload[0];    
        bwc->setJSONSettings(message);
        send_mqtt_cfg_needed = true;
    }
}

/**
 * Connect to MQTT broker, publish Status/MAC/count, and subscribe to keypad topic.
 */
void mqttConnect()
{
    // do not connect if MQTT is not enabled or WiFI not connected
    if (!mqtt_info->useMqtt || (WiFi.status() != WL_CONNECTED))
    {
        return;
    }
    BWC_LOG_P(PSTR("MQTT > connecting\n"),0);

    // Serial.print(F("MQTT > Connecting ... "));
    // We'll connect with a Retained Last Will that updates the 'Status' topic with "Dead" when the device goes offline...
    if (mqttClient->connect(
        mqtt_info->mqttClientId.c_str(), // client_id : the client ID to use when connecting to the server->
        mqtt_info->mqttUsername.c_str(), // username : the username to use. If NULL, no username or password is used (const char[])
        mqtt_info->mqttPassword.c_str(), // password : the password to use. If NULL, no password is used (const char[])setupHA
        (String(mqtt_info->mqttBaseTopic) + F("/Status")).c_str(), // willTopic : the topic to be used by the will message (const char[])
        0, // willQoS : the quality of service to be used by the will message (int : 0,1 or 2)
        1, // willRetain : whether the will should be published with the retain flag (int : 0 or 1)
        "Dead")) // willMessage : the payload of the will message (const char[])
    {
        // Serial.println(F("success!"));
        mqtt_connect_count++;

        // update MQTT every X seconds. (will also be updated on state changes)
        updateMqttTimer->attach(mqtt_info->mqttTelemetryInterval, []{ sendMQTTFlag = true; });

        // These all have the Retained flag set to true, so that the value is stored on the server and can be retrieved at any point
        // Check the 'Status' topic to see that the device is still online before relying on the data from these retained topics
        mqttClient->publish((String(mqtt_info->mqttBaseTopic) + F("/Status")).c_str(), "Alive", true);
        mqttClient->publish((String(mqtt_info->mqttBaseTopic) + F("/MAC_Address")).c_str(), WiFi.macAddress().c_str(), true);                 // Device MAC Address
        mqttClient->publish((String(mqtt_info->mqttBaseTopic) + F("/MQTT_Connect_Count")).c_str(), String(mqtt_connect_count).c_str(), true); // MQTT Connect Count
        mqttClient->loop();

        // Watch the 'command' topic for incoming MQTT messages
        mqttClient->subscribe((String(mqtt_info->mqttBaseTopic) + F("/command")).c_str());
        mqttClient->subscribe((String(mqtt_info->mqttBaseTopic) + F("/command_batch")).c_str());
        mqttClient->subscribe((String(mqtt_info->mqttBaseTopic) + F("/set_config")).c_str());
        mqttClient->loop();

        #ifdef ESP8266
        // mqttClient->publish((String(mqttBaseTopic) + "/reboot_time").c_str(), DateTime.format(DateFormatter::SIMPLE).c_str(), true);
        mqttClient->publish((String(mqtt_info->mqttBaseTopic) + F("/reboot_time")).c_str(), (bwc->reboot_time_str+'Z').c_str(), true);
        mqttClient->publish((String(mqtt_info->mqttBaseTopic) + F("/reboot_reason")).c_str(), ESP.getResetReason().c_str(), true);
        String buttonname;
        buttonname.reserve(32);
        bwc->getButtonName(buttonname);
        mqttClient->publish((String(mqtt_info->mqttBaseTopic) + F("/button")).c_str(), buttonname.c_str(), true);
        mqttClient->loop();
        sendMQTT();
        BWC_LOG_P(PSTR("MQTT > Sending HA discovery"),0);
        mqttClient->setBufferSize(1536);
        setupHA();
        mqttClient->setBufferSize(512);
        mqttClient->loop();
        // Serial.println(F("MQTT Sending config"));
        // sendMQTTConfig();    // Stack smashing if doing this here :-(
        send_mqtt_cfg_needed = true;
        BWC_LOG_P(PSTR("MQTT > connect done\n"),0);
        #endif
    }
    else
    {
        // Serial.print(F("failed, Return Code = "));
        // Serial.println(mqttClient->state()); // states explained in webSocket->js
    }
    BWC_YIELD;
}

time_t getBootTime()
{
    time_t seconds = millis() / 1000;
    time_t result = time(nullptr) - seconds;
    return result;
}

void handleESPInfo()
{
    if (!webauth::requireAuth()) return;
    #ifdef ESP8266
    char stack;
    uint32_t stacksize = stack_start - &stack;
    size_t const BUFSIZE = 1024;
    char response[BUFSIZE];

    char const *response_dram =
    PSTR(
    "Stack size:               %u \n"
    "Free Dram Heap:           %u \n"
    "Min Dram Heap:            %u \n"
    "Max free Dram block size: %u \n\n");

    char const *response_iram =
    PSTR(
    "Free Iram Heap:           %u \n"
    "Max free Iram block size: %u \n\n"
    "Core version:             %s \n"
    "CPU fq:                   %u MHz\n"
    "Cycle count:              %u \n"
    "Free cont stack:          %u \n"
    "Sketch size:              %u \n"
    "Free sketch space:        %u \n"
    );

    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send(200, F("text/plain"), "");

    snprintf_P(response, BUFSIZE, response_dram,
        stacksize,
        ESP.getFreeHeap(),
        heap_water_mark,
        ESP.getMaxFreeBlockSize() );
    server->sendContent(response);
    uint32_t iram_heap; 
    uint32_t iram_maxblock;
    {
        HeapSelectIram ephemeral;
        iram_heap = ESP.getFreeHeap();
        iram_maxblock = ESP.getMaxFreeBlockSize();
    }
    snprintf_P(response, BUFSIZE, response_iram,
        iram_heap,
        iram_maxblock,
        ESP.getCoreVersion().c_str(),
        ESP.getCpuFreqMHz(),
        ESP.getCycleCount(),
        ESP.getFreeContStack(),
        ESP.getSketchSize(),
        ESP.getFreeSketchSpace()
    );
    
    server->sendContent(response);
    server->sendContent("");

    #endif
}

// /api/watchdog-status — runtime audit endpoint. Returns the current
// watchdog state so we can see how often it almost-fires (armCount) without
// it actually restarting. Use this to validate that the thresholds are safe
// before relying on auto-recovery in production.
//
// Fields:
//   armCount             — how many times the watchdog has armed since boot.
//                          A healthy device should see 0 most of the time;
//                          a small non-zero count means transient busy spikes
//                          dipped below threshold but recovered (good — the
//                          guard worked). Climbing fast = tune thresholds.
//   currentlyArmed       — true if max-block is below threshold right now.
//   armedForS            — how long the current arm has lasted (0 if disarmed).
//   uptimeS              — millis()/1000.
//   minUptimeS           — the boot-warmup guard before fires are allowed.
//   maxBlock             — current largest contiguous free DRAM block.
//   thresholdBytes       — the trip-point.
//   timeoutS             — how long fragmented-state must persist to fire.
//   canFireNow           — uptime guard satisfied, so a fire would proceed if armed.
//   lastArmedAtUptimeS   — uptime at which the most recent arm happened.
//   lastArmedMaxBlock    — max-block snapshot at the most recent arm.
void handleWatchdogStatus()
{
    if (!webauth::requireAuth()) return;
    uint32_t maxBlock = ESP.getMaxFreeBlockSize();
    unsigned long nowMs = millis();
    unsigned long armedForS = (lowHeapStartedMs == 0) ? 0UL :
                              (nowMs - lowHeapStartedMs) / 1000UL;
    bool canFireNow = (nowMs >= WATCHDOG_MIN_UPTIME);

    String body;
    body.reserve(320);
    body  = F("{\"armCount\":");        body += wdArmCount;
    body += F(",\"currentlyArmed\":");  body += (lowHeapStartedMs != 0) ? F("true") : F("false");
    body += F(",\"armedForS\":");       body += armedForS;
    body += F(",\"uptimeS\":");         body += (nowMs / 1000UL);
    body += F(",\"minUptimeS\":");      body += (WATCHDOG_MIN_UPTIME / 1000UL);
    body += F(",\"maxBlock\":");        body += maxBlock;
    body += F(",\"thresholdBytes\":");  body += WATCHDOG_MAXBLOCK_THRESHOLD;
    body += F(",\"timeoutS\":");        body += (WATCHDOG_LOW_HEAP_TIMEOUT / 1000UL);
    body += F(",\"canFireNow\":");      body += canFireNow ? F("true") : F("false");
    body += F(",\"lastArmedAtUptimeS\":"); body += (wdLastArmedAtMs / 1000UL);
    body += F(",\"lastArmedMaxBlock\":"); body += wdLastArmedMaxBlock;
    body += F("}");

    server->send(200, F("application/json"), body);
}

void setTemperatureFromSensor()
{
    if(bwc->hasTempSensor)
    { 
            tempSensors->requestTemperatures(); 
            float temperatureC = tempSensors->getTempCByIndex(0);
            //float temperatureF = tempSensors.getTempFByIndex(0);
            //Serial.print(temperatureC);
            //Serial.println("ºC");
            //Serial.print(temperatureF);
            //Serial.println("ºF");

            // Ignore bad reads
            if(temperatureC >= -20.0)
            {
                bwc->setAmbientTemperature(temperatureC, true);
            }
    }
    BWC_YIELD;
}

extern "C" void custom_crash_callback(struct rst_info * rst_info, uint32_t stack, uint32_t stack_end )
{
    File file = LittleFS.open(F("crashlog.txt"), "a");
    String crashinfo;
    crashinfo.reserve(1024);
    char tempstring[64];
    time_t t = time(NULL);
    crashinfo = F("Crashed time: ");
    crashinfo += asctime(gmtime(&t));
    // write crash time to buffer
    uint32_t crashTime = millis();
    crashinfo += F("\nMillis:");
    crashinfo += crashTime;
    crashinfo += '\n';

    // write reset info to buffer
    // REASON_DEFAULT_RST      = 0,    /* normal startup by power on */
    // REASON_WDT_RST          = 1,    /* hardware watch dog reset */
    // REASON_EXCEPTION_RST    = 2,    /* exception reset, GPIO status won’t change */
    // REASON_SOFT_WDT_RST     = 3,    /* software watch dog reset, GPIO status won’t change */
    // REASON_SOFT_RESTART     = 4,    /* software restart ,system_restart , GPIO status won’t change */
    // REASON_DEEP_SLEEP_AWAKE = 5,    /* wake up from deep-sleep */
    // REASON_EXT_SYS_RST      = 6     /* external system reset */
    String reasons[7] = {
        "REASON_DEFAULT_RST",
        "REASON_WDT_RST",
        "REASON_EXCEPTION_RST",
        "REASON_SOFT_WDT_RST",
        "REASON_SOFT_RESTART",
        "REASON_DEEP_SLEEP_AWAKE",
        "REASON_EXT_SYS_RST"
    };

    snprintf(tempstring, 64, "%d (%s)\n", rst_info->reason, reasons[rst_info->reason].c_str());
    crashinfo += F("Reason: ");
    crashinfo += tempstring;

    crashinfo += F("Cause:  ");
    crashinfo += rst_info->exccause;
    crashinfo += F(" (0=cmd invalid, 6=div by zero, 9=unaligned r/w, 28/29=access to invalid addr)\n");

    crashinfo += F("epc1: ");
    snprintf(tempstring, 16, "%08x\n", rst_info->epc1);
    crashinfo += tempstring;

    crashinfo += F("epc2: ");
    snprintf(tempstring, 16, "%08x\n", rst_info->epc2);
    crashinfo += tempstring;

    crashinfo += F("epc3: ");
    snprintf(tempstring, 16, "%08x\n", rst_info->epc3);
    crashinfo += tempstring;

    crashinfo += F("excvaddr: ");
    snprintf(tempstring, 16, "%08x\n", rst_info->excvaddr);
    crashinfo += tempstring;

    crashinfo += F("depc: ");
    snprintf(tempstring, 16, "%08x\n", rst_info->depc);
    crashinfo += tempstring;

    crashinfo += F("\nstack>>>>>>>>>>>>>");

    for (uint32_t iAddress = stack; iAddress < stack_end; iAddress += sizeof(uint32_t*))
    {
        char buf[16];
        snprintf(buf, 16, "\n%08x: ", iAddress);
        crashinfo += buf;
        for(uint32_t i = 0; i < 4; i++)
        {
            uint32_t* pValue = (uint32_t*) iAddress;
            snprintf(buf, 16, "%08x ", *pValue);
            crashinfo += buf;
            crashinfo += ' ';
        }
    }

    crashinfo += F("\n<<<<<<<<<<<<<stack\n\n\n");
    file.print(crashinfo);
    file.close();
    delay(3000);
    // ESP.restart();
}

#include "ha.hpp"
#include "prometheus.hpp"