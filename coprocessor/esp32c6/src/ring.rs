//! A byte ring of `N` bytes, `N` a power of two, holding up to `N - 1`.

pub struct Ring<const N: usize> {
    buf: [u8; N],
    head: usize,
    tail: usize,
}

impl<const N: usize> Ring<N> {
    const MASK: usize = {
        assert!(N.is_power_of_two());
        N - 1
    };

    pub const fn new() -> Self {
        Self { buf: [0; N], head: 0, tail: 0 }
    }

    pub fn clear(&mut self) {
        self.head = 0;
        self.tail = 0;
    }

    pub fn used(&self) -> usize {
        self.head.wrapping_sub(self.tail) & Self::MASK
    }

    pub fn free(&self) -> usize {
        Self::MASK - self.used()
    }

    /// Stores as much of `p` as fits and returns how much that was.
    pub fn push(&mut self, p: &[u8]) -> usize {
        let n = p.len().min(self.free());
        for &b in &p[..n] {
            self.buf[self.head] = b;
            self.head = (self.head + 1) & Self::MASK;
        }
        n
    }

    /// Stores all of `p`, or nothing when it does not fit.
    pub fn push_all(&mut self, p: &[u8]) -> bool {
        p.len() <= self.free() && self.push(p) == p.len()
    }

    pub fn pop(&mut self, p: &mut [u8]) -> usize {
        let n = p.len().min(self.used());
        for b in &mut p[..n] {
            *b = self.buf[self.tail];
            self.tail = (self.tail + 1) & Self::MASK;
        }
        n
    }
}

impl<const N: usize> Default for Ring<N> {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::Ring;

    #[test]
    fn wraps_and_bounds() {
        let mut r = Ring::<8>::new();
        assert_eq!(r.push(b"abcdefghij"), 7);
        assert!(!r.push_all(b"x"));
        let mut out = [0u8; 5];
        assert_eq!(r.pop(&mut out), 5);
        assert_eq!(&out, b"abcde");
        assert!(r.push_all(b"klmno"));
        assert!(!r.push_all(b"p"));
        let mut out = [0u8; 16];
        assert_eq!(r.pop(&mut out), 7);
        assert_eq!(&out[..7], b"fgklmno");
        assert_eq!((r.used(), r.free()), (0, 7));
    }
}
