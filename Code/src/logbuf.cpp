#include "logbuf.h"
#include <stdarg.h>

namespace logbuf {

namespace {

constexpr size_t MAX_LINES = 100;
constexpr size_t MAX_LINE_LEN = 200;

String lines[MAX_LINES];
size_t head    = 0;   // next write position
size_t kept    = 0;   // total lines retained (capped at MAX_LINES)

}   // namespace

void setup() {
    for (size_t i = 0; i < MAX_LINES; i++) lines[i] = String();
    head = 0;
    kept = 0;
}

void write(const String &line) {
    // Prefix with monotonic ms so the user can tell a stuck loop from a
    // restart in the dump. Wraps at uint32_t (~49 days) — fine for our
    // debug needs.
    String prefixed;
    prefixed.reserve(line.length() + 12);
    prefixed = "[";
    prefixed += String(millis());
    prefixed += "ms] ";
    prefixed += line;

    lines[head] = prefixed;
    head = (head + 1) % MAX_LINES;
    if (kept < MAX_LINES) kept++;

    // Tee to Serial as well. Useless on the production spa adapter
    // (USB-isolated) but harmless and useful if anyone runs this on
    // a dev board with USB-serial actually wired.
    Serial.println(prefixed);
}

void write(const char *line) {
    write(String(line ? line : ""));
}

void writef(const char *fmt, ...) {
    char buf[MAX_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write(String(buf));
}

String dump() {
    // Reserve enough for the typical full ring (~12 KB). Avoids the
    // realloc-cascade that String concatenation triggers otherwise.
    String out;
    out.reserve(kept * 100);
    // Oldest-first iteration so the dump reads top-to-bottom like a log.
    size_t start = (kept < MAX_LINES) ? 0 : head;
    for (size_t i = 0; i < kept; i++) {
        size_t idx = (start + i) % MAX_LINES;
        out += lines[idx];
        out += '\n';
    }
    return out;
}

size_t lineCount() { return kept; }

}   // namespace logbuf
