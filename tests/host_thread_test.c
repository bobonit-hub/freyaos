/*
 * Freya - host test for the thread table.
 *
 * The switch itself is PendSV and does not run here.  What does run is
 * the policy: a thread must be named, its priority is a small integer,
 * a higher priority becomes the one running, and stop removes it.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "freya.h"

uint8_t __worker_stacks[4 * 1024] __attribute__((aligned(8)));

__asm__(
    ".globl __worker_stack_size\n"
    ".globl __worker_count\n"
    ".set __worker_stack_size, 1024\n"
    ".set __worker_count, 4\n"
);

static char s_out[2048];
static int s_outn;
static int checks, fails;
static int s_stop_requested;

app_state_t g_app;

static void pass(const char *what) { checks++; printf("  ok    %s\n", what); }
static void fail(const char *what)
{
    checks++;
    fails++;
    printf("  FAIL  %s\n", what);
    if (s_out[0]) printf("        output: %s\n", s_out);
}

void uart_putc(char c)
{
    if (s_outn < (int)sizeof s_out - 1) s_out[s_outn++] = c;
    s_out[s_outn] = '\0';
}
void uart_puts(const char *s) { while (*s) uart_putc(*s++); }

uint32_t sys_ticks(void) { return 1000; }
int app_in_handler(void) { return 0; }
int app_switch_blocked(void) { return 0; }
int app_should_stop(void) { return s_stop_requested; }
void app_request_stop(void) { s_stop_requested = 1; }
void shell_poll_runtime(void) { }

static void expect(const char *what, int cond)
{
    if (cond) pass(what);
    else fail(what);
}

static void worker(void *arg) { (void)arg; }

int main(void)
{
    int lo, hi, rc;

    printf("names and priorities\n");
    thread_init();
    g_app.running = 1;

    expect("the shell is thread 0", thread_self() == 0);
    expect("an empty name is refused",
           thread_create("", 1, worker, NULL) == FREYA_ERR_ARG);
    expect("a missing name is refused",
           thread_create(NULL, 1, worker, NULL) == FREYA_ERR_ARG);
    expect("a blank name is refused",
           thread_create("  ", 1, worker, NULL) == FREYA_ERR_ARG);
    expect("a missing function is refused",
           thread_create("work", 1, NULL, NULL) == FREYA_ERR_ARG);
    expect("priority below the floor is refused",
           thread_create("work", -1, worker, NULL) == FREYA_ERR_ARG);
    expect("priority above the ceiling is refused",
           thread_create("work", 8, worker, NULL) == FREYA_ERR_ARG);

    printf("scheduling\n");
    lo = thread_create("lo", 1, worker, NULL);
    expect("a named thread is created", lo > 0);
    expect("priority 1 runs ahead of the shell", thread_self() == lo);
    hi = thread_create("hi", 5, worker, NULL);
    expect("a second thread is created", hi > lo);
    expect("the higher priority is now running", thread_self() == hi);
    expect("a duplicate name is refused",
           thread_create("lo", 1, worker, NULL) == FREYA_ERR_BUSY);
    expect("the shell's name is taken",
           thread_create("shell", 1, worker, NULL) == FREYA_ERR_BUSY);
    expect("priority 0 is accepted while a run is on",
           thread_create("bg", FREYA_PRIO_MIN, worker, NULL) > 0);
    {
        int top = thread_create("top", FREYA_PRIO_MAX, worker, NULL);
        expect("priority 7 is accepted and runs", top > 0 && thread_self() == top);
    }

    printf("stop and list\n");
    s_outn = 0;
    s_out[0] = '\0';
    thread_list();
    expect("the list names the running thread", strstr(s_out, "top") != NULL);
    expect("the list names lo", strstr(s_out, "lo") != NULL);
    expect("the list names the shell", strstr(s_out, "shell") != NULL);
    expect("the list names idle", strstr(s_out, "idle") != NULL);

    rc = thread_stop_name("top");
    expect("stop of the running thread succeeds", rc == 0);
    expect("the previous thread is running again", thread_self() != 0);
    expect("the stopped thread is gone", thread_stop_name("top") == FREYA_ERR_ARG);
    expect("an unknown thread is refused",
           thread_stop_name("nosuch") == FREYA_ERR_ARG);
    expect("idle cannot be stopped", thread_stop_name("idle") == FREYA_ERR_BUSY);

    s_stop_requested = 0;
    thread_run_begin("demo");
    expect("stopping the program thread requests a stop",
           thread_stop_name("demo") == 0 && s_stop_requested);
    thread_run_end();
    g_app.running = 0;
    expect("the shell is back", thread_self() == 0);
    expect("the shell cannot be stopped",
           thread_stop_name("shell") == FREYA_ERR_BUSY);
    expect("a thread cannot be created with no program running",
           thread_create("late", 1, worker, NULL) == FREYA_ERR_HANDLER);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
