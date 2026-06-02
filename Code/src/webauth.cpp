#include "webauth.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

#ifdef ESP8266
extern ESP8266WebServer *server;
#else
extern WebServer server;
#endif

namespace webauth {

namespace {

// Persistent state — written to LittleFS so reboots don't log the user out
// and so the password survives OTA updates that erase Preferences but keep
// the filesystem.
constexpr const char *AUTH_FILE = "/auth.json";
constexpr int  SESSION_TOKEN_LEN = 24;        // 24 lowercase-alphanum chars ≈ 124 bits
constexpr int  MAX_LOGIN_ATTEMPTS = 5;
constexpr uint32_t LOCKOUT_MS = 30000;        // 30 s after 5 fails
constexpr const char *DEFAULT_PASSWORD = "spa";

char password[33] = "";                       // null-terminated, max 32 chars
char sessionToken[SESSION_TOKEN_LEN + 1] = "";

uint8_t  loginFailures = 0;
uint32_t lockoutStartMs = 0;

void generateToken() {
    static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
#ifdef ESP8266
    // ESP8266 hardware RNG via the wifi RF — non-zero on any board that's
    // booted the network stack, which we have by the time setup() runs.
    auto rnd = []() { return *((volatile uint32_t *)0x3FF20E44); };
#else
    auto rnd = []() { return esp_random(); };
#endif
    for (int i = 0; i < SESSION_TOKEN_LEN; i++) {
        sessionToken[i] = chars[rnd() % (sizeof(chars) - 1)];
    }
    sessionToken[SESSION_TOKEN_LEN] = '\0';
}

void persist() {
    StaticJsonDocument<256> doc;
    doc["password"] = password;
    doc["session"]  = sessionToken;
    File f = LittleFS.open(AUTH_FILE, "w");
    if (!f) return;
    serializeJson(doc, f);
    f.close();
}

bool load() {
    if (!LittleFS.exists(AUTH_FILE)) return false;
    File f = LittleFS.open(AUTH_FILE, "r");
    if (!f) return false;
    StaticJsonDocument<256> doc;
    auto err = deserializeJson(doc, f);
    f.close();
    if (err) return false;
    const char *pw = doc["password"] | "";
    const char *sn = doc["session"]  | "";
    if (pw[0] != '\0' && strlen(pw) <= 32) {
        strncpy(password, pw, sizeof(password) - 1);
    }
    if (strlen(sn) == SESSION_TOKEN_LEN) {
        strncpy(sessionToken, sn, sizeof(sessionToken) - 1);
    }
    return true;
}

bool isAuthenticated() {
#ifdef ESP8266
    if (!server->hasHeader(F("Cookie"))) return false;
    String cookie = server->header(F("Cookie"));
#else
    if (!server.hasHeader("Cookie")) return false;
    String cookie = server.header("Cookie");
#endif
    String expected = String("session=") + sessionToken;
    int idx = cookie.indexOf(expected);
    if (idx < 0) return false;
    int endIdx = idx + expected.length();
    if (endIdx < (int)cookie.length() &&
        cookie[endIdx] != ';' && cookie[endIdx] != ' ') {
        return false;   // partial-prefix match — reject
    }
    return true;
}

void redirectLogin() {
#ifdef ESP8266
    server->sendHeader(F("Location"), F("/login.html"));
    server->send(302, F("text/plain"), F("Redirecting…"));
#else
    server.sendHeader("Location", "/login.html");
    server.send(302, "text/plain", "Redirecting…");
#endif
}

}   // namespace

void setup() {
    // LittleFS must already be mounted by main.cpp's setup() before this runs.
    generateToken();             // Fresh token on every boot — clears stale sessions
    if (!load()) {
        // First boot: seed with the default password + the just-generated token
        strncpy(password, DEFAULT_PASSWORD, sizeof(password) - 1);
        persist();
    } else {
        // Loaded password but persist the new session token alongside it
        persist();
    }
}

bool isAuthRouteUri(const String &uri) {
    return uri == F("/login.html") || uri == F("/login.html.gz") ||
           uri == F("/login")      || uri == F("/logout");
}

bool requireAuth() {
    if (isAuthenticated()) return true;
#ifdef ESP8266
    const String &uri = server->uri();
#else
    const String &uri = server.uri();
#endif
    // Auth-related routes themselves must never be gated (chicken/egg).
    if (isAuthRouteUri(uri)) return true;
    // Anything under /api/ or POSTs to JSON endpoints get a 401; HTML
    // pages get redirected. The /get*/ and /set*/ routes are the spa's
    // JSON-ish endpoints — treat them as API.
    bool jsonish = uri.startsWith(F("/api/")) ||
                   uri.startsWith(F("/get"))  ||
                   uri.startsWith(F("/set"))  ||
                   uri.startsWith(F("/add"))  ||
                   uri.startsWith(F("/edit")) ||
                   uri.startsWith(F("/del"))  ||
                   uri == F("/metrics");
    if (jsonish) {
#ifdef ESP8266
        server->send(401, F("application/json"),
                     F("{\"error\":\"auth required\"}"));
#else
        server.send(401, "application/json", "{\"error\":\"auth required\"}");
#endif
    } else {
        redirectLogin();
    }
    return false;
}

void handleLoginPost() {
    // Lockout window — site-wide, not per-IP. Simple but sufficient for a LAN
    // device. Resets after LOCKOUT_MS, after which the counter clears.
    if (loginFailures >= MAX_LOGIN_ATTEMPTS) {
        uint32_t elapsed = millis() - lockoutStartMs;
        if (elapsed < LOCKOUT_MS) {
#ifdef ESP8266
            server->send(429, F("text/plain"),
                         F("Too many attempts — try again in 30 s"));
#else
            server.send(429, "text/plain", "Too many attempts — try again in 30 s");
#endif
            return;
        }
        loginFailures = 0;
    }
#ifdef ESP8266
    String pw = server->arg("password");
#else
    String pw = server.arg("password");
#endif
    if (pw.length() > 0 && pw == password) {
        loginFailures = 0;
        String cookie = String("session=") + sessionToken +
                        "; Path=/; HttpOnly; SameSite=Strict; Max-Age=604800";
#ifdef ESP8266
        server->sendHeader(F("Set-Cookie"), cookie);
        server->sendHeader(F("Location"), F("/"));
        server->send(302, F("text/plain"), F("OK"));
#else
        server.sendHeader("Set-Cookie", cookie);
        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "OK");
#endif
    } else {
        if (++loginFailures >= MAX_LOGIN_ATTEMPTS) lockoutStartMs = millis();
#ifdef ESP8266
        server->sendHeader(F("Location"), F("/login.html?error=1"));
        server->send(302, F("text/plain"), F("Bad password"));
#else
        server.sendHeader("Location", "/login.html?error=1");
        server.send(302, "text/plain", "Bad password");
#endif
    }
}

void handleLogout() {
#ifdef ESP8266
    server->sendHeader(F("Set-Cookie"),
                       F("session=; Path=/; HttpOnly; Max-Age=0"));
    server->sendHeader(F("Location"), F("/login.html"));
    server->send(302, F("text/plain"), F("Logged out"));
#else
    server.sendHeader("Set-Cookie", "session=; Path=/; HttpOnly; Max-Age=0");
    server.sendHeader("Location", "/login.html");
    server.send(302, "text/plain", "Logged out");
#endif
    // Rotate the token so the now-deleted cookie can never be re-used even
    // if it leaked. Forces all other browsers signed in with the same token
    // to also re-auth — acceptable trade-off for a single-user device.
    generateToken();
    persist();
}

bool setPassword(const String &newPw) {
    if (newPw.length() == 0 || newPw.length() > 32) return false;
    strncpy(password, newPw.c_str(), sizeof(password) - 1);
    password[sizeof(password) - 1] = '\0';
    persist();
    return true;
}

const char *currentPassword() { return password; }

}   // namespace webauth
