"""PL/M lexer. Identifiers are case-insensitive and '$' is ignored inside them."""

import os

from plm.error import PlmError

KEYWORDS = {
    "DECLARE", "LITERALLY", "PROCEDURE", "DO", "END", "IF", "THEN", "ELSE",
    "GOTO", "GO", "TO", "BY", "CASE", "WHILE", "RETURN", "CALL", "HALT",
    "PUBLIC", "EXTERNAL", "REENTRANT", "INITIAL", "DATA", "BASED", "AT",
    "BYTE", "WORD", "DWORD", "INTEGER", "ADDRESS", "POINTER", "STRUCTURE",
    "LABEL", "AND", "OR", "XOR", "NOT", "MOD", "PLUS", "MINUS", "REAL",
    "FLOAT",
}


class Token:
    def __init__(self, kind, value, line, col, filename):
        self.kind = kind
        self.value = value
        self.line = line
        self.col = col
        self.filename = filename

    def __repr__(self):
        return f"Token({self.kind!r}, {self.value!r}, {self.line})"


class _Frame:
    def __init__(self, text, filename, macro=None):
        self.text = text
        self.filename = filename
        self.i = 0
        self.line = 1
        self.col = 1
        self.macro = macro


class Lexer:
    def __init__(self, text, filename, include_dirs):
        self.frames = [_Frame(text, filename)]
        self.include_dirs = list(include_dirs)
        self.macros = {}
        self.buf = []
        self.include_stack = []
        self._expanding = []

    def define_macro(self, name, text):
        self.macros[name] = text

    def peek(self):
        while True:
            if not self.buf:
                self.buf.append(self._next_raw())
            tok = self.buf[0]
            if (tok.kind == "ID" and tok.value in self.macros
                    and tok.value not in self._expanding):
                self.buf.pop(0)
                self._splice(tok.value, tok.line, tok.col, tok.filename)
                continue
            return tok

    def next(self):
        tok = self.peek()
        self.buf.pop(0)
        return tok

    def _frame(self):
        return self.frames[-1]

    def _getc(self):
        fr = self._frame()
        if fr.i >= len(fr.text):
            return ""
        ch = fr.text[fr.i]
        fr.i += 1
        if ch == "\n":
            fr.line += 1
            fr.col = 1
        else:
            fr.col += 1
        return ch

    def _peekc(self, k=0):
        fr = self._frame()
        j = fr.i + k
        if j >= len(fr.text):
            return ""
        return fr.text[j]

    def _starts(self, word):
        fr = self._frame()
        return fr.text[fr.i:fr.i + len(word)].upper() == word

    def _next_raw(self):
        while True:
            fr = self._frame()
            if fr.i >= len(fr.text):
                tok = self._eof_frame()
                if tok is not None:
                    return tok
                continue
            self._skip_space_and_comments()
            fr = self._frame()
            if fr.i >= len(fr.text):
                continue
            if self._starts("$INCLUDE"):
                self._do_include()
                continue
            return self._read_token()

    def _skip_space_and_comments(self):
        while True:
            ch = self._peekc()
            if ch == "" :
                return
            if ch in " \t\r\n":
                self._getc()
                continue
            if ch == "/" and self._peekc(1) == "*":
                fr = self._frame()
                line, col = fr.line, fr.col
                self._getc()
                self._getc()
                while True:
                    c = self._getc()
                    if c == "":
                        raise PlmError("unterminated comment", line, col, fr.filename)
                    if c == "*" and self._peekc() == "/":
                        self._getc()
                        break
                continue
            return

    def _do_include(self):
        fr = self._frame()
        line, col, filename = fr.line, fr.col, fr.filename
        # consume $INCLUDE
        for _ in range(8):
            self._getc()
        self._skip_space_and_comments()
        if self._peekc() != "(":
            raise PlmError("$INCLUDE expects (filename)", line, col, filename)
        self._getc()
        self._skip_space_and_comments()
        if self._peekc() == "'":
            self._getc()
            name = []
            while True:
                c = self._getc()
                if c == "":
                    raise PlmError("unterminated include name", line, col, filename)
                if c == "'":
                    break
                name.append(c)
            inc = "".join(name)
        else:
            name = []
            while self._peekc() not in ("", ")"):
                name.append(self._getc())
            inc = "".join(name).strip()
        self._skip_space_and_comments()
        if self._peekc() != ")":
            raise PlmError("$INCLUDE missing ')'", line, col, filename)
        self._getc()
        if not inc:
            raise PlmError("empty $INCLUDE", line, col, filename)
        path = self._find_include(inc, filename)
        if path in self.include_stack:
            raise PlmError(f"recursive $INCLUDE of {inc}", line, col, filename)
        with open(path, "r", encoding="utf-8") as fh:
            text = fh.read()
        self.include_stack.append(path)
        # Pop the include from the stack when its frame is exhausted. The
        # frame itself does not know that, so wrap the text with a sentinel
        # handled by storing the path on the frame via a side channel.
        self.frames.append(_Frame(text, path, macro=None))
        self.frames[-1].include_path = path

    def _find_include(self, inc, current):
        dirs = []
        if current and not current.startswith("<"):
            dirs.append(os.path.dirname(os.path.abspath(current)))
        dirs.extend(self.include_dirs)
        for d in dirs:
            cand = os.path.join(d, inc)
            if os.path.isfile(cand):
                return os.path.abspath(cand)
        raise PlmError(f"include file not found: {inc}")

    def _eof_frame(self):
        fr = self.frames[-1]
        path = getattr(fr, "include_path", None)
        if len(self.frames) == 1:
            return Token("EOF", None, fr.line, fr.col, fr.filename)
        self.frames.pop()
        if path and path in self.include_stack:
            self.include_stack.remove(path)
        return None

    def _read_token(self):
        fr = self._frame()
        line, col, filename = fr.line, fr.col, fr.filename
        ch = self._getc()
        two = ch + self._peekc()
        if two in ("<=", ">=", "<>"):
            self._getc()
            return Token(two, two, line, col, filename)
        if ch in "()+-*/,;:=.<>":
            return Token(ch, ch, line, col, filename)
        if ch == "'":
            return self._string(line, col, filename)
        if ch.isdigit():
            return self._number(ch, line, col, filename)
        if ch.isalpha() or ch == "$" or ch == "_":
            return self._ident(ch, line, col, filename)
        raise PlmError(f"invalid character {ch!r}", line, col, filename)

    def _string(self, line, col, filename):
        chars = []
        while True:
            c = self._getc()
            if c == "":
                raise PlmError("unterminated string", line, col, filename)
            if c == "'":
                if self._peekc() == "'":
                    self._getc()
                    chars.append("'")
                    continue
                break
            chars.append(c)
        return Token("STR", "".join(chars), line, col, filename)

    def _number(self, first, line, col, filename):
        text = first
        while self._peekc().isdigit() or self._peekc().isalpha():
            # Stop before a float dot; the dot itself is diagnosed below.
            text += self._getc()
        if self._peekc() == "." and self._peekc(1).isdigit():
            raise PlmError("floating point is not supported", line, col, filename)
        suffix = ""
        body = text
        if len(text) > 1 and text[-1].upper() in "HDBOQ":
            suffix = text[-1].upper()
            body = text[:-1]
        if not body or not body[0].isdigit():
            raise PlmError(f"bad number {text}", line, col, filename)
        if suffix in ("", "D"):
            base = 10
        elif suffix == "H":
            base = 16
        elif suffix == "B":
            base = 2
        elif suffix in ("O", "Q"):
            base = 8
        else:
            raise PlmError(f"bad number suffix {suffix}", line, col, filename)
        try:
            value = int(body, base)
        except ValueError:
            raise PlmError(f"bad number {text}", line, col, filename)
        if value > 0xFFFFFFFF:
            raise PlmError("number does not fit in 32 bits", line, col, filename)
        return Token("NUM", value, line, col, filename)

    def _ident(self, first, line, col, filename):
        raw = first
        while True:
            c = self._peekc()
            if c.isalnum() or c in "$_":
                raw += self._getc()
            else:
                break
        name = raw.replace("$", "").upper()
        if not name or not (name[0].isalpha() or name[0] == "_"):
            raise PlmError(f"invalid identifier {raw}", line, col, filename)
        kind = name if name in KEYWORDS else "ID"
        return Token(kind, name, line, col, filename)

    def _splice(self, name, line, col, filename):
        """Queue every token of a LITERALLY body in place of the name."""
        if name in self._expanding:
            raise PlmError(f"recursive LITERALLY {name}", line, col, filename)
        self._expanding.append(name)
        saved_frames = self.frames
        saved_buf = self.buf
        self.frames = [_Frame(self.macros[name], filename, macro=name)]
        self.buf = []
        tokens = []
        try:
            while True:
                tok = self.peek()
                if tok.kind == "EOF":
                    break
                tokens.append(self.next())
        finally:
            self.frames = saved_frames
            self.buf = saved_buf
            self._expanding.pop()
        if not tokens:
            raise PlmError(f"empty LITERALLY {name}", line, col, filename)
        self.buf = tokens + self.buf
