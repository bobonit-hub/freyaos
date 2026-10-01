//! Threads of the run.  They share the run and end with it; each has
//! `FREYA_THREAD_STACK` bytes of stack, which is little for `core::fmt`,
//! so keep their formatting short.  None of these may be called from a
//! pin or timer handler.

use core::ffi::{c_int, c_void, CStr};

use crate::{check, raw, require, sys, Result};

/// A thread id, as `thread_self()` reports it.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct ThreadId(pub c_int);

unsafe extern "C" fn trampoline(arg: *mut c_void) {
    // SAFETY: arg is the fn() that spawn() was given.
    let f = unsafe { core::mem::transmute::<*mut c_void, fn()>(arg) };
    f()
}

/// Start `f` as a thread named `name` (what the console stops it by) at
/// `priority`, `FREYA_PRIO_MIN..=FREYA_PRIO_MAX`; the program's own main
/// runs at `FREYA_PRIO_NORMAL`.  Returning from `f` ends the thread.
pub fn spawn(name: &CStr, priority: c_int, f: fn()) -> Result<ThreadId> {
    require!(thread_create);
    check(unsafe { (raw().thread_create)(name.as_ptr(), priority, Some(trampoline), f as *mut c_void) })
        .map(ThreadId)
}

/// End the calling thread.  From the program's main thread this ends the
/// run the way returning would.
pub fn thread_exit() -> ! {
    if !crate::api_has!(raw(), thread_exit) {
        crate::api().exit(sys::FREYA_EXIT_OK);
    }
    unsafe { (raw().thread_exit)() }
}

pub fn thread_yield() {
    if crate::api_has!(raw(), thread_yield) {
        unsafe { (raw().thread_yield)() }
    }
}

/// Sleep `ms` milliseconds.  `Err` when the run was asked to stop meanwhile,
/// which is the thread's cue to return.
pub fn thread_sleep(ms: u32) -> Result<()> {
    require!(thread_sleep);
    match unsafe { (raw().thread_sleep)(ms) } {
        -1 => Err(crate::Error::IO),
        r => check(r).map(drop),
    }
}

pub fn thread_self() -> Result<ThreadId> {
    require!(thread_self);
    check(unsafe { (raw().thread_self)() }).map(ThreadId)
}
