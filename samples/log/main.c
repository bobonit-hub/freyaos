/*
 * log - write one line at every Freya log level.
 *
 *     run log.bin              emit error, warn, info and debug
 *     run log.bin debug        raise the filter first, so debug is kept
 *     run log.bin error        only the error line is stored
 *
 * Lines go to /freya.log (or the console if the card is not mounted).
 * 'cat /freya.log' shows which names actually landed.
 */
#include "freya_api.h"

static int streq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return *a == *b;
}

static const char *level_name(int level)
{
    switch (level) {
    case FREYA_LOG_OFF:   return "off";
    case FREYA_LOG_ERROR: return "error";
    case FREYA_LOG_WARN:  return "warn";
    case FREYA_LOG_INFO:  return "info";
    case FREYA_LOG_DEBUG: return "debug";
    default:              return "?";
    }
}

static int parse_level(const char *s, int *out)
{
    int v = 0, digits = 0;

    if (streq(s, "off"))        { *out = FREYA_LOG_OFF;   return 0; }
    if (streq(s, "error") || streq(s, "err")) { *out = FREYA_LOG_ERROR; return 0; }
    if (streq(s, "warn") || streq(s, "warning")) { *out = FREYA_LOG_WARN; return 0; }
    if (streq(s, "info"))       { *out = FREYA_LOG_INFO;  return 0; }
    if (streq(s, "debug"))      { *out = FREYA_LOG_DEBUG; return 0; }

    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s++ - '0');
        digits++;
    }
    if (!digits || *s || v < FREYA_LOG_OFF || v > FREYA_LOG_DEBUG)
        return -1;
    *out = v;
    return 0;
}

int app_main(const freya_api_t *api, int argc, char **argv)
{
    int before, after;

    before = api->get_log_level();
    after  = before;

    if (argc > 1) {
        if (parse_level(argv[1], &after) != 0) {
            api->printf("log: unknown level '%s'\r\n", argv[1]);
            api->puts("usage: run log.bin [off|error|warn|info|debug | 0..4]\r\n");
            return 1;
        }
        api->set_log_level(after);
        after = api->get_log_level();
    }

    api->printf("log sample: filter is %s (%d)", level_name(after), after);
    if (after != before)
        api->printf(", was %s (%d)", level_name(before), before);
    api->puts("\r\n");
    api->puts("writing ERROR, WARN, INFO and DEBUG ...\r\n");

    api->log(FREYA_LOG_ERROR, "sample error: something failed, code=%d", 7);
    api->log(FREYA_LOG_WARN,  "sample warn: the card is %u%% full", 90u);
    api->log(FREYA_LOG_INFO,  "sample info: tick %u ms", api->ticks_ms());
    api->log(FREYA_LOG_DEBUG, "sample debug: argc=%d argv0=%s",
             argc, (argc > 0) ? argv[0] : "-");

    api->puts("done. lines at or below the filter are in /freya.log\r\n");
    api->puts("(cat /freya.log   or, if the card is out, they already printed)\r\n");
    return 0;
}
