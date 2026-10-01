//! Files and directories on the card (or the Black Pill's SPI flash).

use core::ffi::{c_char, c_int, c_void};
use core::fmt;

use crate::{raw, str_from_c, sys};

/// A filesystem error, one of the `sys::FAT_ERR_*` codes.
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct FsError(pub c_int);

impl FsError {
    pub const IO: FsError = FsError(sys::FAT_ERR_IO);
    pub const NOFS: FsError = FsError(sys::FAT_ERR_NOFS);
    pub const NOENT: FsError = FsError(sys::FAT_ERR_NOENT);
    pub const EXIST: FsError = FsError(sys::FAT_ERR_EXIST);
    pub const NOSPC: FsError = FsError(sys::FAT_ERR_NOSPC);
    pub const INVAL: FsError = FsError(sys::FAT_ERR_INVAL);
    pub const NOTDIR: FsError = FsError(sys::FAT_ERR_NOTDIR);
    pub const ISDIR: FsError = FsError(sys::FAT_ERR_ISDIR);
    pub const NOTEMPTY: FsError = FsError(sys::FAT_ERR_NOTEMPTY);
    pub const NOFILE: FsError = FsError(sys::FAT_ERR_NOFILE);
    pub const RDONLY: FsError = FsError(sys::FAT_ERR_RDONLY);

    pub fn name(self) -> &'static str {
        match self.0 {
            sys::FAT_ERR_IO => "i/o error",
            sys::FAT_ERR_NOFS => "no filesystem",
            sys::FAT_ERR_NOENT => "not found",
            sys::FAT_ERR_EXIST => "already exists",
            sys::FAT_ERR_NOSPC => "no space",
            sys::FAT_ERR_INVAL => "invalid",
            sys::FAT_ERR_NOTDIR => "not a directory",
            sys::FAT_ERR_ISDIR => "is a directory",
            sys::FAT_ERR_NOTEMPTY => "directory not empty",
            sys::FAT_ERR_NOFILE => "no free descriptor",
            sys::FAT_ERR_RDONLY => "read only",
            _ => "unknown error",
        }
    }
}

impl fmt::Debug for FsError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} ({})", self.name(), self.0)
    }
}

impl fmt::Display for FsError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.name())
    }
}

impl core::error::Error for FsError {}

pub type Result<T> = core::result::Result<T, FsError>;

fn check(r: c_int) -> Result<c_int> {
    if r < 0 { Err(FsError(r)) } else { Ok(r) }
}

/// Run `f` with `path` as a NUL-terminated string.  A path is at most
/// `sys::FAT_MAX_PATH - 1` bytes, which is what the kernel takes.
fn with_path<R>(path: &str, f: impl FnOnce(*const c_char) -> R) -> Result<R> {
    let mut buf = [0u8; sys::FAT_MAX_PATH];
    let b = path.as_bytes();
    if b.len() >= buf.len() || b.contains(&0) {
        return Err(FsError::INVAL);
    }
    buf[..b.len()].copy_from_slice(b);
    Ok(f(buf.as_ptr() as *const c_char))
}

/// `open()` flags; combine them with `|`.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct OpenFlags(pub c_int);

impl OpenFlags {
    pub const READ: OpenFlags = OpenFlags(sys::FREYA_O_RDONLY);
    pub const WRITE: OpenFlags = OpenFlags(sys::FREYA_O_WRONLY);
    pub const READ_WRITE: OpenFlags = OpenFlags(sys::FREYA_O_RDWR);
    pub const CREATE: OpenFlags = OpenFlags(sys::FREYA_O_CREATE);
    pub const TRUNC: OpenFlags = OpenFlags(sys::FREYA_O_TRUNC);
    pub const APPEND: OpenFlags = OpenFlags(sys::FREYA_O_APPEND);
}

impl core::ops::BitOr for OpenFlags {
    type Output = OpenFlags;
    fn bitor(self, rhs: OpenFlags) -> OpenFlags {
        OpenFlags(self.0 | rhs.0)
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum SeekFrom {
    Start(u32),
    Current(i32),
    End(i32),
}

/// An open file, closed when dropped.  `write!` works on it.
pub struct File {
    fd: c_int,
}

impl File {
    pub fn open(path: &str, flags: OpenFlags) -> Result<File> {
        let fd = with_path(path, |p| unsafe { (raw().open)(p, flags.0) })?;
        check(fd).map(|fd| File { fd })
    }
    /// Open for writing, creating the file or emptying it.
    pub fn create(path: &str) -> Result<File> {
        File::open(path, OpenFlags::WRITE | OpenFlags::CREATE | OpenFlags::TRUNC)
    }

    /// Read up to `buf.len()` bytes; 0 is the end of the file.
    pub fn read(&mut self, buf: &mut [u8]) -> Result<usize> {
        let len = buf.len().min(c_int::MAX as usize) as c_int;
        check(unsafe { (raw().read)(self.fd, buf.as_mut_ptr() as *mut c_void, len) }).map(|n| n as usize)
    }
    pub fn write(&mut self, buf: &[u8]) -> Result<usize> {
        let len = buf.len().min(c_int::MAX as usize) as c_int;
        check(unsafe { (raw().write)(self.fd, buf.as_ptr() as *const c_void, len) }).map(|n| n as usize)
    }
    /// Write all of `buf`, or fail with `NOSPC` when the card fills up.
    pub fn write_all(&mut self, mut buf: &[u8]) -> Result<()> {
        while !buf.is_empty() {
            match self.write(buf)? {
                0 => return Err(FsError::NOSPC),
                n => buf = &buf[n..],
            }
        }
        Ok(())
    }
    /// Seek, and return the new position.
    pub fn seek(&mut self, pos: SeekFrom) -> Result<u32> {
        let (off, whence) = match pos {
            SeekFrom::Start(o) => (o as i32, sys::FREYA_SEEK_SET),
            SeekFrom::Current(o) => (o, sys::FREYA_SEEK_CUR),
            SeekFrom::End(o) => (o, sys::FREYA_SEEK_END),
        };
        check(unsafe { (raw().seek)(self.fd, off, whence) })?;
        self.tell()
    }
    pub fn tell(&self) -> Result<u32> {
        check(unsafe { (raw().tell)(self.fd) }).map(|n| n as u32)
    }
    pub fn len(&self) -> Result<u32> {
        check(unsafe { (raw().fsize)(self.fd) }).map(|n| n as u32)
    }
    pub fn is_empty(&self) -> Result<bool> {
        self.len().map(|n| n == 0)
    }
    /// The kernel's descriptor, for the raw calls.
    pub fn fd(&self) -> c_int {
        self.fd
    }
}

impl Drop for File {
    fn drop(&mut self) {
        unsafe { (raw().close)(self.fd) };
    }
}

impl fmt::Write for File {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        self.write_all(s.as_bytes()).map_err(|_| fmt::Error)
    }
}

pub fn unlink(path: &str) -> Result<()> {
    check(with_path(path, |p| unsafe { (raw().unlink)(p) })?).map(drop)
}

pub fn mkdir(path: &str) -> Result<()> {
    check(with_path(path, |p| unsafe { (raw().mkdir)(p) })?).map(drop)
}

/// Rename within the filesystem; no data is copied.
pub fn rename(old: &str, new: &str) -> Result<()> {
    if !crate::api_has!(raw(), rename) {
        return Err(FsError::INVAL);
    }
    let r = with_path(old, |o| with_path(new, |n| unsafe { (raw().rename)(o, n) }))??;
    check(r).map(drop)
}

/// One entry of a directory listing.
#[derive(Clone, Copy)]
pub struct DirEntry(sys::freya_stat_t);

impl DirEntry {
    pub fn name(&self) -> &str {
        str_from_c(&self.0.name)
    }
    pub fn size(&self) -> u32 {
        self.0.size
    }
    pub fn is_dir(&self) -> bool {
        self.0.is_dir != 0
    }
}

/// An open directory: an iterator over its entries, closed when dropped.
pub struct Dir {
    dd: c_int,
}

impl Dir {
    pub fn open(path: &str) -> Result<Dir> {
        let dd = with_path(path, |p| unsafe { (raw().opendir)(p) })?;
        check(dd).map(|dd| Dir { dd })
    }
}

impl Iterator for Dir {
    type Item = DirEntry;
    fn next(&mut self) -> Option<DirEntry> {
        let mut st = sys::freya_stat_t { name: [0; 64], size: 0, is_dir: 0, pad: [0; 3] };
        if unsafe { (raw().readdir)(self.dd, &mut st) } == 0 { Some(DirEntry(st)) } else { None }
    }
}

impl Drop for Dir {
    fn drop(&mut self) {
        unsafe { (raw().closedir)(self.dd) };
    }
}
