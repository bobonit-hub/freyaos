# Threads

A program can start threads. They are not POSIX threads: there is no
`pthread_create`, no attributes block and no cancellation state. A thread
is a name, a numeric priority and a function. A script has its own,
described in [shell.md](shell.md): two functions, sharing the interpreter,
running until `sleep` or `yield`. Those are not these threads, and they
are refused while a program is running.

```c
static void blink(void *arg)
{
    const freya_api_t *api = arg;

    while (!api->should_stop()) {
        api->led(1);
        if (api->thread_sleep(200) != 0) return;
    }
}

int app_main(const freya_api_t *api, int argc, char **argv)
{
    int id = api->thread_create("blink", 2, blink, (void *)api);

    if (id < 0) return FREYA_EXIT_FAIL;
    while (!api->should_stop()) api->thread_sleep(1000);
    return 0;
}
```

`samples/threads` is the small version of that.

The name is mandatory. An empty name, a name containing a space, or a name
longer than `FREYA_THREAD_NAME_MAX` (16) is refused with `FREYA_ERR_ARG`.
So is a priority outside `FREYA_PRIO_MIN` .. `FREYA_PRIO_MAX` (0 .. 7).
A name that is already in use, or a board with no stack left for another
thread, comes back as `FREYA_ERR_BUSY`. Creating a thread from a pin or
timer handler, or when no program is running, is `FREYA_ERR_HANDLER`.

A larger priority runs ahead of a smaller one. Equal priorities take
turns, ten milliseconds at a time, and also whenever one of them yields
or sleeps. The program's own `app_main` is a thread for the length of the
run, at `FREYA_PRIO_NORMAL` (1), under the program's name. The shell is
that same thread when nothing is running, at priority 0. Idle sits below
0 and runs only when every other thread is blocked.

Each thread a program creates has `FREYA_THREAD_STACK` (1024) bytes of
stack. The main thread keeps the shell's stack. The Blue Pill has room
for two of those threads; the Black Pill has room for four. A thread that
writes past the bottom of its stack ends the run.

`thread_exit()` does not return. Called from a thread the program
created, it ends that thread. Called from the main thread, it ends the
run the way returning from `app_main` does. `thread_sleep(0)` and
`thread_yield()` give the CPU to whoever is waiting. `thread_sleep`
returns 0, or -1 when the run has been asked to stop. `thread_self()` is
the caller's id. The shell is 0, idle is 1, and threads a program creates
count up from 2.

Threads live for the run. When `app_main` returns, `exit()` is called, or
the run is stopped, every thread the program created is stopped too.

From the console, `threads` lists them:

```
freya:/> threads
  id  pri  state    name
   0    0  running  shell
   1   -1  ready    idle
```

`stop blink` stops the thread of that name and leaves the rest of the run
going. `stop` with no name still stops the whole program, and with it
every thread. Stopping the program's own name does that too. The shell
and idle cannot be stopped. Ctrl-C stops the run the way it always has,
whichever thread happens to hold the CPU.

A line typed while a program runs is read from the scheduler, so `threads`,
`stop` and `help` work even when the program is not blocked. Any other
command is refused until the run ends: the card and the loader are not
re-entered under a live thread. Characters are not taken while the program
itself is waiting on the console.

None of the thread calls may be used from a pin or timer handler. A
handler is not a thread, and it still has the limits in
[interrupts.md](interrupts.md).
