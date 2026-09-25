"""Recursive-descent parser for a PL/M-86 subset."""

from plm.error import PlmError


class Node:
    def __init__(self, kind, **kw):
        self.kind = kind
        for k, v in kw.items():
            setattr(self, k, v)


class Parser:
    def __init__(self, lexer):
        self.lx = lexer
        self.tok = lexer.next()

    def _at(self, node=None):
        return self.tok.line, self.tok.col, self.tok.filename

    def _err(self, msg):
        raise PlmError(msg, self.tok.line, self.tok.col, self.tok.filename)

    def _advance(self):
        prev = self.tok
        self.tok = self.lx.next()
        return prev

    def _check(self, kind):
        return self.tok.kind == kind

    def _match(self, kind):
        if self._check(kind):
            self._advance()
            return True
        return False

    def _expect(self, kind):
        if not self._check(kind):
            self._err(f"expected {kind}, found {self.tok.kind}")
        return self._advance()

    def _ident(self):
        if not self._check("ID"):
            self._err(f"expected a name, found {self.tok.kind}")
        tok = self._advance()
        return tok.value

    def parse_module(self):
        line, col, filename = self.tok.line, self.tok.col, self.tok.filename
        name = None
        if self._check("ID"):
            name = self._ident()
            self._expect(":")
        self._expect("DO")
        self._expect(";")
        decls, stmts = self._body()
        self._expect("END")
        end = self._ident() if self._check("ID") else None
        self._expect(";")
        if not self._check("EOF"):
            self._err("extra tokens after the module")
        if name is None:
            name = end or "APP"
        if end is not None and name != end:
            raise PlmError(f"END {end} does not match {name}", line, col, filename)
        return Node("module", name=name, decls=decls, stmts=stmts,
                    line=line, col=col, filename=filename)

    def _body(self):
        decls = []
        while True:
            if self._check("DECLARE"):
                decls.extend(self._declare())
            elif self._check("ID") and self._is_proc():
                decls.append(self._procedure())
            else:
                break
        stmts = []
        while not self._check("END") and not self._check("EOF"):
            if self._check("DECLARE") or (self._check("ID") and self._is_proc()):
                self._err("declaration after a statement")
            stmts.append(self._stmt())
        return decls, stmts

    def _is_proc(self):
        # Look at the token after this identifier without consuming it
        # permanently: the lexer buffer is not enough, so peek via a
        # one-token saved state. The next token is already current.
        # We need ID ':' PROCEDURE. Current is ID. Save and read ahead.
        saved = self.tok
        nxt = self.lx.peek()
        if nxt.kind != ":":
            return False
        # consume into a side buffer by reading two tokens and pushing back
        colon = self.lx.next()
        proc = self.lx.peek()
        self.lx.buf.insert(0, colon)
        return proc.kind == "PROCEDURE"

    def _declare(self):
        self._expect("DECLARE")
        items = [self._dcl_item()]
        while self._match(","):
            items.append(self._dcl_item())
        self._expect(";")
        return items

    def _dcl_item(self):
        line, col, filename = self.tok.line, self.tok.col, self.tok.filename
        if self._match("("):
            names = [self._ident()]
            while self._match(","):
                names.append(self._ident())
            self._expect(")")
            early = self._early_attrs()
            typ = self._type()
            attrs = self._attrs()
            self._merge_attrs(attrs, early)
            return Node("vars", names=names, typ=typ, dim=None, attrs=attrs,
                        line=line, col=col, filename=filename)
        name = self._ident()
        if self._match("LITERALLY"):
            if not self._check("STR"):
                self._err("LITERALLY expects a quoted replacement")
            text = self._advance().value
            self.lx.define_macro(name, text)
            return Node("literally", name=name, line=line, col=col, filename=filename)
        dim = None
        if self._match("("):
            if self._match("*"):
                dim = "*"
            else:
                dim = self._expr()
            self._expect(")")
        early = self._early_attrs()
        typ = self._type()
        attrs = self._attrs()
        self._merge_attrs(attrs, early)
        return Node("vars", names=[name], typ=typ, dim=dim, attrs=attrs,
                    line=line, col=col, filename=filename)

    def _type(self):
        line, col, filename = self.tok.line, self.tok.col, self.tok.filename
        if self._match("REAL") or self._check("FLOAT"):
            self._err("floating point is not supported")
        for kind in ("BYTE", "WORD", "DWORD", "INTEGER", "ADDRESS", "POINTER", "LABEL"):
            if self._match(kind):
                return Node("type", name="ADDRESS" if kind == "POINTER" else kind,
                            line=line, col=col, filename=filename)
        if self._match("STRUCTURE"):
            self._expect("(")
            fields = []
            while not self._check(")"):
                fields.append(self._member())
                if not self._match(","):
                    break
            self._expect(")")
            return Node("type", name="STRUCTURE", fields=fields,
                        line=line, col=col, filename=filename)
        self._err(f"expected a type, found {self.tok.kind}")

    def _member(self):
        line, col, filename = self.tok.line, self.tok.col, self.tok.filename
        name = self._ident()
        dim = None
        if self._match("("):
            if self._match("*"):
                self._err("structure members need an explicit dimension")
            dim = self._expr()
            self._expect(")")
        typ = self._type()
        return Node("member", name=name, dim=dim, typ=typ,
                    line=line, col=col, filename=filename)

    def _early_attrs(self):
        """BASED, AT, PUBLIC and EXTERNAL may precede the type."""
        found = {"public": False, "external": False, "based": None, "at": None}
        while True:
            if self._match("PUBLIC"):
                found["public"] = True
            elif self._match("EXTERNAL"):
                found["external"] = True
            elif self._match("BASED"):
                found["based"] = self._ident()
            elif self._match("AT"):
                self._expect("(")
                found["at"] = self._expr()
                self._expect(")")
            else:
                return found

    def _merge_attrs(self, attrs, early):
        for key in ("public", "external"):
            attrs[key] = attrs[key] or early[key]
        if early["based"]:
            attrs["based"] = early["based"]
        if early["at"] is not None:
            attrs["at"] = early["at"]

    def _attrs(self):
        attrs = {"public": False, "external": False, "based": None,
                 "at": None, "section": None, "init": None}
        while True:
            if self._match("PUBLIC"):
                attrs["public"] = True
            elif self._match("EXTERNAL"):
                attrs["external"] = True
            elif self._match("BASED"):
                attrs["based"] = self._ident()
            elif self._match("AT"):
                self._expect("(")
                attrs["at"] = self._expr()
                self._expect(")")
            elif self._check("DATA") or self._check("INITIAL"):
                attrs["section"] = "rodata" if self.tok.kind == "DATA" else "data"
                self._advance()
                self._expect("(")
                attrs["init"] = self._init_list()
                self._expect(")")
            else:
                break
        return attrs

    def _init_list(self):
        items = [self._init_item()]
        while self._match(","):
            items.append(self._init_item())
        return items

    def _init_item(self):
        if self._check("STR"):
            text = self._advance().value
            node = Node("str", value=text, line=0, col=0, filename="")
        else:
            node = self._expr()
        if self._match("("):
            inner = self._init_item()
            self._expect(")")
            return Node("rep", count=node, item=inner, line=node.line, col=node.col,
                        filename=node.filename)
        return node

    def _procedure(self):
        line, col, filename = self.tok.line, self.tok.col, self.tok.filename
        name = self._ident()
        self._expect(":")
        self._expect("PROCEDURE")
        params = []
        if self._match("("):
            if not self._check(")"):
                params.append(self._ident())
                while self._match(","):
                    params.append(self._ident())
            self._expect(")")
        ret = None
        public = external = reentrant = False
        while True:
            if self.tok.kind in ("BYTE", "WORD", "DWORD", "INTEGER", "ADDRESS",
                                 "POINTER", "REAL", "FLOAT") and ret is None:
                ret = self._type()
            elif self._match("PUBLIC"):
                public = True
            elif self._match("EXTERNAL"):
                external = True
            elif self._match("REENTRANT"):
                reentrant = True
            else:
                break
        self._expect(";")
        if external:
            decls, stmts = self._body()
            if stmts:
                self._err("an EXTERNAL procedure has no body")
        else:
            decls, stmts = self._body()
        self._expect("END")
        end = self._ident() if self._check("ID") else None
        self._expect(";")
        if end != name:
            raise PlmError(f"END {end} does not match PROCEDURE {name}",
                           line, col, filename)
        return Node("proc", name=name, params=params, ret=ret, public=public,
                    external=external, reentrant=reentrant, decls=decls,
                    stmts=stmts, line=line, col=col, filename=filename)

    def _stmt(self):
        line, col, filename = self.tok.line, self.tok.col, self.tok.filename
        if self._match(";"):
            return Node("empty", line=line, col=col, filename=filename)
        if self._match("IF"):
            cond = self._expr()
            self._expect("THEN")
            then = self._stmt()
            els = self._stmt() if self._match("ELSE") else None
            return Node("if", cond=cond, then=then, els=els,
                        line=line, col=col, filename=filename)
        if self._match("DO"):
            return self._do(line, col, filename)
        if self._match("GOTO") or (self._match("GO") and self._expect("TO")):
            name = self._ident()
            self._expect(";")
            return Node("goto", name=name, line=line, col=col, filename=filename)
        if self._match("RETURN"):
            expr = None if self._check(";") else self._expr()
            self._expect(";")
            return Node("return", expr=expr, line=line, col=col, filename=filename)
        if self._match("CALL"):
            call = self._postfix()
            self._expect(";")
            return Node("call", call=call, line=line, col=col, filename=filename)
        if self._match("HALT"):
            self._expect(";")
            return Node("halt", line=line, col=col, filename=filename)
        if self._check("ID"):
            saved_kind = self.lx.peek().kind
            if saved_kind == ":":
                name = self._ident()
                self._expect(":")
                inner = self._stmt()
                return Node("label", name=name, stmt=inner,
                            line=line, col=col, filename=filename)
            dst = self._postfix()
            self._expect("=")
            src = self._expr()
            self._expect(";")
            return Node("assign", dst=dst, src=src, line=line, col=col, filename=filename)
        self._err(f"expected a statement, found {self.tok.kind}")

    def _do(self, line, col, filename):
        if self._match(";"):
            body = self._until_end()
            self._expect("END")
            self._skip_end_name()
            return Node("block", body=body, line=line, col=col, filename=filename)
        if self._match("WHILE"):
            cond = self._expr()
            self._expect(";")
            body = self._until_end()
            self._expect("END")
            self._skip_end_name()
            return Node("while", cond=cond, body=body, line=line, col=col, filename=filename)
        if self._match("CASE"):
            expr = self._expr()
            self._expect(";")
            arms = []
            while not self._check("END") and not self._check("EOF"):
                arms.append(self._stmt())
            self._expect("END")
            self._skip_end_name()
            return Node("case", expr=expr, arms=arms, line=line, col=col, filename=filename)
        var = self._ident()
        self._expect("=")
        start = self._expr()
        self._expect("TO")
        limit = self._expr()
        step = self._expr() if self._match("BY") else None
        self._expect(";")
        body = self._until_end()
        self._expect("END")
        self._skip_end_name()
        return Node("doloop", var=var, start=start, limit=limit, step=step,
                    body=body, line=line, col=col, filename=filename)

    def _until_end(self):
        stmts = []
        while not self._check("END") and not self._check("EOF"):
            if self._check("DECLARE") or (self._check("ID") and self._is_proc()):
                self._err("declaration after a statement")
            stmts.append(self._stmt())
        return stmts

    def _skip_end_name(self):
        if self._check("ID"):
            self._advance()
        self._expect(";")

    def _postfix(self):
        line, col, filename = self.tok.line, self.tok.col, self.tok.filename
        name = self._ident()
        args = None
        if self._match("("):
            args = []
            if not self._check(")"):
                args.append(self._expr())
                while self._match(","):
                    args.append(self._expr())
            self._expect(")")
        fields = []
        while self._match("."):
            fields.append(self._ident())
        return Node("ref", name=name, args=args, fields=fields,
                    line=line, col=col, filename=filename)

    def _expr(self):
        return self._or()

    def _binary(self, higher, ops):
        node = higher()
        while self.tok.kind in ops:
            op = self._advance().kind
            rhs = higher()
            node = Node("binop", op=op, left=node, right=rhs,
                        line=node.line, col=node.col, filename=node.filename)
        return node

    def _or(self):
        return self._binary(self._xor, ("OR",))

    def _xor(self):
        return self._binary(self._and, ("XOR",))

    def _and(self):
        return self._binary(self._rel, ("AND",))

    def _rel(self):
        return self._binary(self._add, ("<", "<=", ">", ">=", "<>", "="))

    def _add(self):
        return self._binary(self._mul, ("+", "-", "PLUS", "MINUS"))

    def _mul(self):
        return self._binary(self._unary, ("*", "/", "MOD"))

    def _unary(self):
        if self.tok.kind in ("+", "-", "NOT"):
            op = self._advance().kind
            expr = self._unary()
            return Node("unop", op=op, expr=expr, line=expr.line, col=expr.col,
                        filename=expr.filename)
        return self._primary()

    def _primary(self):
        line, col, filename = self.tok.line, self.tok.col, self.tok.filename
        if self._match("("):
            node = self._expr()
            self._expect(")")
            return node
        if self._check("NUM"):
            value = self._advance().value
            return Node("num", value=value, line=line, col=col, filename=filename)
        if self._check("STR"):
            value = self._advance().value
            return Node("str", value=value, line=line, col=col, filename=filename)
        if self._match("."):
            ref = self._postfix()
            return Node("addr", ref=ref, line=line, col=col, filename=filename)
        if self._check("ID"):
            return self._postfix()
        self._err(f"expected an expression, found {self.tok.kind}")
