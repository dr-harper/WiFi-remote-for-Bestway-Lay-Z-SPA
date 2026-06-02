#pragma once

// Cookie-based session auth for the spa web UI.
//
// Ported 2026-06-01 from the HeatSync (Samsung NASA controller) project's
// two-token auth, simplified for the ESP8266 + synchronous ESP8266WebServer
// environment used here:
//   • single in-memory session token, persisted to /auth.json on LittleFS so
//     reboots don't kick the user out
//   • password stored in the same auth.json (plaintext — this is a LAN-only
//     device, hashing would be theatre)
//   • per-IP-less lockout: 5 failed login attempts → 30 s site-wide cooldown
//     (good enough for a LAN device; if you ever want per-IP, swap a map in)
//   • requireAuth() is the gate every handler should call first:
//       void handleX() { if (!requireAuth()) return; … }
//   • /api/ routes return 401 JSON; HTML page requests get redirected to /login.html
//
// First-boot default password is "spa". A banner on first login should
// prompt the user to change it via the Web Config page (when that wiring
// lands).

#include <Arduino.h>
#ifdef ESP8266
#include <ESP8266WebServer.h>
#else
#include <WebServer.h>
#endif

namespace webauth {

void setup();                          // Load password + session token from LittleFS
bool requireAuth();                    // Gate — true if request is authed; emits redirect/401 otherwise
void handleLoginPost();                // POST /login — validates password, sets cookie
void handleLogout();                   // POST /logout — clears cookie
bool isAuthRouteUri(const String &uri);// True if uri is /login.html, /login, /logout — never gated

// Password management (called by setwebconfig handler once you wire it in).
bool setPassword(const String &newPw);
const char *currentPassword();         // Exposed for the "change password" UI to confirm old pw

}   // namespace webauth
