/*
 * Freya - board power, the part that is not a pin.
 *
 * src/power.c decides what has to happen around the rail: close files,
 * unmount, refuse a handler, leave a domain that is already in the
 * asked-for state alone.  The rail itself is sd_power(), stubbed here.
 */
#include <stdio.h>

#include "freya.h"
#include "fat.h"

static int checks, fails;

static int s_powered = 1;
static int s_mounted;
static int s_handler;
static int s_closed;
static int s_unmounted;
static int s_rail;          /* last level handed to sd_power, or -1 */

static void check(const char *what, int expected, int got)
{
    checks++;
    if (expected == got) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s: expected %d, got %d\n", what, expected, got);
        fails++;
    }
}

int  sd_powered(void)          { return s_powered; }
int  sd_power(int on)          { s_rail = on; s_powered = on; return 0; }
void fs_close_all(void)        { s_closed++; }
int  fat_mounted(void)         { return s_mounted; }
void fat_unmount(void)         { s_unmounted++; s_mounted = 0; }
void spiflash_unmount(void)    { }
int  app_in_handler(void)      { return s_handler; }

int main(void)
{
    int rc;

    s_mounted = 1;
    s_rail = -1;
    rc = board_power(FREYA_PWR_SD, 0);
    check("off returns the state it found", 1, rc);
    check("off closes open files", 1, s_closed);
    check("off unmounts", 1, s_unmounted);
    check("off drops the rail", 0, s_rail);
    check("the socket is then off", 0, sd_powered());

    s_rail = -1;
    rc = board_power(FREYA_PWR_SD, 0);
    check("off again returns off", 0, rc);
    check("and does not touch the rail", -1, s_rail);
    check("or close files a second time", 1, s_closed);

    s_rail = -1;
    rc = board_power(FREYA_PWR_SD, 1);
    check("on returns the state it found", 0, rc);
    check("on raises the rail", 1, s_rail);
    check("on does not unmount", 1, s_unmounted);
    check("the socket is then on", 1, sd_powered());

    s_rail = -1;
    rc = board_power(FREYA_PWR_SD, 1);
    check("on again returns on", 1, rc);
    check("and does not touch the rail", -1, s_rail);

    s_mounted = 1;
    s_powered = 1;
    rc = board_power(99, 0);
    check("an unknown domain is refused", FREYA_ERR_ARG, rc);
    check("and the socket stays on", 1, sd_powered());
    check("and stays mounted", 1, s_mounted);

    rc = board_power(FREYA_PWR_SD, 2);
    check("a level other than 0 or 1 is refused", FREYA_ERR_ARG, rc);

    s_handler = 1;
    rc = board_power(FREYA_PWR_SD, 0);
    check("a handler is refused", FREYA_ERR_HANDLER, rc);
    check("and the socket stays on", 1, sd_powered());
    check("and the files stay open", 1, s_closed);
    s_handler = 0;

    s_mounted = 0;
    s_unmounted = 0;
    rc = board_power(FREYA_PWR_SD, 0);
    check("off with nothing mounted still returns on", 1, rc);
    check("and does not unmount", 0, s_unmounted);
    check("but still closes files and drops the rail", 0, sd_powered());

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
