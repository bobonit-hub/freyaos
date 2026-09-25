"""Bind a PL/M module and emit a Cortex-M3 assembler listing.

The listing is a Freya flash program for the Blue Pill program region.
It is linked with boards/bluepill/app_flash.ld and started with runflash.
"""

from plm.error import PlmError

# Blue Pill program flash, include/freya_api.h FREYA_APP_FLASH_ADDR.
FLASH_ADDR = 0x0800C080
FLASH_SIZE = 37760
ABI_VERSION = 3
APP_MAGIC = 0x41595246
XIP_FLAG = 1

# freya_api_t function-pointer offsets. Variadic calls are omitted.
API = {
    "PUTC": (8, 1),
    "PUTS": (12, 1),
    "GETC": (20, 0),
    "GETC_TIMEOUT": (24, 1),
    "KBHIT": (28, 0),
    "MALLOC": (32, 1),
    "FREE": (36, 1),
    "TICKS_MS": (40, 0),
    "DELAY_MS": (44, 1),
    "SHOULD_STOP": (48, 0),
    "YIELD": (52, 0),
    "EXIT": (56, 1),
    "OPEN": (60, 2),
    "CLOSE": (64, 1),
    "READ": (68, 3),
    "WRITE": (72, 3),
    "SEEK": (76, 3),
    "TELL": (80, 1),
    "FSIZE": (84, 1),
    "UNLINK": (88, 1),
    "MKDIR": (92, 1),
    "OPENDIR": (96, 1),
    "READDIR": (100, 2),
    "CLOSEDIR": (104, 1),
    "LED": (108, 1),
    "CPU_HZ": (112, 0),
    "RENAME": (116, 2),
}

BUILTINS = {"LENGTH", "LAST", "SIZE", "LOW", "HIGH", "DOUBLE",
            "SHL", "SHR", "ROL", "ROR", "MOVE"}


class Ty:
    def __init__(self, kind, size, align, signed=False, elem=None,
                 length=0, fields=None):
        self.kind = kind
        self.size = size
        self.align = align
        self.signed = signed
        self.elem = elem
        self.length = length
        self.fields = fields or []

    def is_int(self):
        return self.kind in ("byte", "word", "integer", "dword", "address", "const")

    def is_agg(self):
        return self.kind in ("array", "struct")


BYTE = Ty("byte", 1, 1)
WORD = Ty("word", 2, 2)
INTEGER = Ty("integer", 4, 4, signed=True)
DWORD = Ty("dword", 4, 4)
ADDRESS = Ty("address", 4, 4)
CONST = Ty("const", 4, 4, signed=True)
NAMED = {"BYTE": BYTE, "WORD": WORD, "INTEGER": INTEGER,
         "DWORD": DWORD, "ADDRESS": ADDRESS}


def align_up(n, a):
    return (n + a - 1) & ~(a - 1)


class Sym:
    def __init__(self, name):
        self.name = name
        self.skind = "var"
        self.ty = None
        self.storage = "bss"
        self.asm = None
        self.public = False
        self.by_ref = False
        self.frame_off = 0
        self.at_addr = None
        self.base = None
        self.base_name = None
        self.init = None
        self.const = False
        self.params = []
        self.ret = None
        self.reentrant = False
        self.external = False
        self.proc_node = None
        self.scope = None


class Scope:
    def __init__(self, parent=None):
        self.parent = parent
        self.syms = {}

    def add(self, sym):
        if sym.name in self.syms:
            raise PlmError(f"{sym.name} is already declared")
        self.syms[sym.name] = sym
        return sym

    def lookup(self, name):
        s = self
        while s is not None:
            if name in s.syms:
                return s.syms[name]
            s = s.parent
        return None


class Codegen:
    def __init__(self, module, stack=2048):
        self.module = module
        self.stack = stack
        self.lines = []
        self.rodata = []
        self.data = []
        self.bss = []
        self.nlabel = 0
        self.ntmp = 0
        self.nstr = 0
        self.scope = Scope()
        self.proc = None
        self.exit_label = None
        self.defined_labels = set()

    def generate(self):
        self._seed()
        self._bind_decls(self.module.decls, self.scope, [])
        self._emit_header()
        self._emit_text_start()
        self._emit_main()
        for sym in list(self.scope.syms.values()):
            if sym.skind == "proc":
                self._emit_proc(sym)
        self._emit_api_wrappers()
        self._emit_sections()
        return "\n".join(self.lines) + "\n"

    # ----------------------------------------------------------- binding

    def _seed(self):
        for name, asm, ty in (
            ("API", "plm_api", ADDRESS),
            ("ARGC", "plm_argc", INTEGER),
            ("ARGV", "plm_argv", ADDRESS),
        ):
            s = Sym(name)
            s.ty = ty
            s.storage = "bss"
            s.asm = asm
            s.scope = self.scope
            self.scope.add(s)
        self.bss.append("\t.balign 4")
        self.bss.append("plm_api:\n\t.space 4")
        self.bss.append("plm_argc:\n\t.space 4")
        self.bss.append("plm_argv:\n\t.space 4")

    def _err(self, node, msg):
        raise PlmError(msg, getattr(node, "line", None),
                       getattr(node, "col", None),
                       getattr(node, "filename", None))

    def _bind_decls(self, decls, scope, path):
        procs = []
        for d in decls:
            if d.kind == "literally":
                continue
            if d.kind == "proc":
                sym = Sym(d.name)
                sym.skind = "proc"
                sym.public = d.public
                sym.external = d.external
                sym.reentrant = d.reentrant or d.external
                sym.ret = self._bind_type(d.ret, scope) if d.ret else None
                sym.proc_node = d
                sym.asm = "plm_proc_" + "_".join(path + [d.name])
                sym.scope = scope
                try:
                    scope.add(sym)
                except PlmError:
                    self._err(d, f"{d.name} is already declared")
                procs.append(sym)
            elif d.kind == "vars":
                self._bind_vars(d, scope, path)
        for sym in procs:
            self._bind_proc_body(sym, path)

    def _bind_proc_body(self, sym, path):
        node = sym.proc_node
        if sym.reentrant and len(node.params) > 4:
            self._err(node, "a REENTRANT procedure has at most four parameters")
        prev = getattr(self, "binding_proc", None)
        self.binding_proc = sym
        scope = Scope(sym.scope)
        sym.params = []
        for pname in node.params:
            p = Sym(pname)
            p.skind = "param"
            p.scope = scope
            try:
                scope.add(p)
            except PlmError:
                self._err(node, f"parameter {pname} is repeated")
            sym.params.append(p)
        self._bind_decls(node.decls, scope, path + [sym.name])
        for p in sym.params:
            if p.ty is None:
                self._err(node, f"parameter {p.name} is not declared")
            p.by_ref = p.ty.is_agg()
            if sym.reentrant:
                p.storage = "frame"
            else:
                p.storage = "bss"
                p.asm = sym.asm + "_" + p.name
                self.bss.append(f"\t.balign 4")
                self.bss.append(f"{p.asm}:\n\t.space 4")
        if sym.reentrant:
            self._layout_frame(sym, scope)
        if node.external and sym.name in API:
            expect = API[sym.name][1]
            if len(sym.params) != expect:
                self._err(node, f"{sym.name} expects {expect} parameters")
        sym.inner = scope
        self.binding_proc = prev

    def _layout_frame(self, sym, scope):
        cursor = 0
        for i, p in enumerate(sym.params):
            p.frame_off = (i + 1) * 4
            cursor = p.frame_off
        for s in scope.syms.values():
            if s.storage == "frame" and s.skind != "param":
                continue
            if s.skind == "var" and s.storage == "frame":
                cursor = align_up(cursor, s.ty.align)
                cursor += s.ty.size
                s.frame_off = cursor
        # parameters occupy the first nargs words; locals were placed after
        # only if storage is frame. Recompute locals cleanly.
        cursor = len(sym.params) * 4
        for s in scope.syms.values():
            if s.skind == "var" and s.storage == "frame":
                cursor = align_up(cursor, max(s.ty.align, 1))
                cursor += s.ty.size
                s.frame_off = cursor
        sym.frame_size = align_up(max(cursor, len(sym.params) * 4), 8)

    def _bind_vars(self, d, scope, path):
        if d.typ.name == "LABEL":
            for name in d.names:
                s = Sym(name)
                s.skind = "label"
                s.scope = scope
                try:
                    scope.add(s)
                except PlmError:
                    self._err(d, f"{name} is already declared")
            return
        base_ty = self._bind_type(d.typ, scope)
        for name in d.names:
            if name in ("API", "ARGC", "ARGV") and scope.parent is None:
                self._err(d, f"{name} is reserved")
            sym = Sym(name)
            sym.public = d.attrs["public"]
            sym.scope = scope
            sym.base_name = d.attrs["based"]
            dim = d.dim
            length = None
            if dim == "*":
                length = None
            elif dim is not None:
                length = self._const(dim, scope)
                if length < 0:
                    self._err(d, "negative dimension")
            ty = base_ty
            if length is not None:
                ty = Ty("array", length * base_ty.size, base_ty.align,
                        elem=base_ty, length=length)
            sym.ty = ty
            prefix = "plm_v_" + ("_".join(path) + "_" if path else "")
            sym.asm = prefix + name
            attrs = d.attrs
            existing = scope.syms.get(name)
            if existing is not None and existing.skind == "param":
                existing.ty = ty
                existing.public = False
                existing._init_ast = None
                existing._star = False
                existing._elem = base_ty
                if attrs["init"] or attrs["based"] or attrs["at"] or attrs["external"]:
                    self._err(d, "a parameter is a plain declaration")
                continue
            if attrs["external"]:
                sym.storage = "extern"
            elif attrs["at"] is not None:
                sym.storage = "abs"
                sym.at_addr = self._const(attrs["at"], scope) & 0xFFFFFFFF
            elif attrs["based"]:
                sym.storage = "based"
            elif getattr(self, "binding_proc", None) and self.binding_proc.reentrant:
                sym.storage = "frame"
            else:
                sym.storage = "bss"
            if attrs["init"] is not None and sym.storage in ("extern", "abs", "based", "frame"):
                self._err(d, "this variable cannot have an initialiser")
            sym._init_ast = attrs["init"]
            sym._init_node = d
            sym._elem = base_ty
            sym._star = dim == "*"
            if attrs["section"] == "rodata":
                sym.const = True
            try:
                scope.add(sym)
            except PlmError:
                self._err(d, f"{name} is already declared")
        # The loop above fails for parameters because the name exists.
        # Handle parameters explicitly: if name is an existing param, decorate it.

    def _bind_type(self, node, scope):
        if node is None:
            return None
        if node.name in NAMED:
            return NAMED[node.name]
        if node.name == "LABEL":
            return None
        if node.name != "STRUCTURE":
            self._err(node, f"unknown type {node.name}")
        fields = []
        off = 0
        align = 1
        for m in node.fields:
            mty = self._bind_type(m.typ, scope)
            if m.dim is not None:
                n = self._const(m.dim, scope)
                mty = Ty("array", n * mty.size, mty.align, elem=mty, length=n)
            off = align_up(off, mty.align)
            fields.append({"name": m.name, "ty": mty, "off": off})
            off += mty.size
            align = max(align, mty.align)
        size = align_up(off, align) if off else 0
        return Ty("struct", size, align, fields=fields)

    def _finish_inits(self, scope):
        for sym in scope.syms.values():
            if sym.skind != "var":
                continue
            if sym.base_name:
                base = scope.lookup(sym.base_name)
                if base is None or base.ty is None or base.ty.kind != "address":
                    raise PlmError(f"{sym.name} is BASED on a non-ADDRESS")
                sym.base = base
            init = getattr(sym, "_init_ast", None)
            if init is None and sym._star if hasattr(sym, "_star") else False:
                raise PlmError(f"{sym.name} needs an initialiser to size it")
            if not hasattr(sym, "_star"):
                continue
            if sym._star and init is None:
                raise PlmError(f"{sym.name} needs an initialiser")
            if init is None:
                if sym.storage == "bss" and sym.ty is not None:
                    self._emit_space(sym)
                continue
            blob = self._encode_init(sym, init, scope)
            if sym._star:
                elem = sym._elem
                if len(blob) % elem.size:
                    raise PlmError(f"{sym.name} initialiser is not a whole number of elements")
                n = len(blob) // elem.size
                sym.ty = Ty("array", len(blob), elem.align, elem=elem, length=n)
            elif sym.ty.is_agg():
                if len(blob) > sym.ty.size:
                    raise PlmError(f"{sym.name} initialiser is too long")
                blob = blob + bytes(sym.ty.size - len(blob))
            else:
                if len(blob) != sym.ty.size:
                    # scalar encoded as one element; allow a short list already checked
                    pass
            sym.init = blob
            if sym.const or sym.storage == "rodata":
                sym.storage = "rodata"
                self._emit_bytes(self.rodata, sym, blob)
            else:
                sym.storage = "data"
                self._emit_bytes(self.data, sym, blob)

    def _emit_space(self, sym):
        if sym.storage != "bss" or sym.asm is None:
            return
        self.bss.append(f"\t.balign {max(sym.ty.align, 1)}")
        line = f"{sym.asm}:"
        if sym.public:
            self.bss.append(f"\t.global {sym.asm}")
        self.bss.append(line)
        self.bss.append(f"\t.space {sym.ty.size}")

    def _emit_bytes(self, bucket, sym, blob):
        bucket.append(f"\t.balign {max(sym.ty.align, 1)}")
        if sym.public:
            bucket.append(f"\t.global {sym.asm}")
        bucket.append(f"{sym.asm}:")
        if not blob:
            bucket.append("\t.space 0")
            return
        nums = ", ".join(str(b) for b in blob)
        bucket.append(f"\t.byte {nums}")

    def _encode_init(self, sym, items, scope):
        elem = sym.ty.elem if sym.ty is not None and sym.ty.kind == "array" else sym.ty
        if sym._star:
            elem = sym._elem
        if elem is None:
            raise PlmError(f"{sym.name} has no type")
        if elem.kind == "struct":
            raise PlmError("structure initialisers are not supported")
        out = bytearray()
        for item in self._flatten_init(items, elem, scope):
            out += item
        if not sym._star and sym.ty is not None and not sym.ty.is_agg():
            if len(out) != elem.size:
                raise PlmError(f"{sym.name} initialiser has the wrong length")
        return bytes(out)

    def _flatten_init(self, items, elem, scope):
        # items is a list of nodes, or we pass the list from attrs
        seq = []
        for item in items:
            if item.kind == "rep":
                n = self._const(item.count, scope)
                inner = self._flatten_init([item.item], elem, scope)
                for _ in range(n):
                    seq.extend(inner)
            elif item.kind == "str":
                if elem.kind != "byte":
                    raise PlmError("a string initialiser needs a BYTE variable")
                for ch in item.value:
                    seq.append(bytes([ord(ch) & 0xFF]))
            else:
                v = self._const(item, scope)
                seq.append(self._enc(v, elem))
        return seq

    def _enc(self, value, ty):
        v = value & ((1 << (ty.size * 8)) - 1)
        return v.to_bytes(ty.size, "little")

    def _const(self, node, scope):
        if node.kind == "num":
            return node.value
        if node.kind == "str":
            if len(node.value) == 1:
                return ord(node.value)
            self._err(node, "string is not a scalar constant")
        if node.kind == "unop":
            v = self._const(node.expr, scope)
            if node.op == "+":
                return v
            if node.op == "-":
                return -v
            if node.op == "NOT":
                return (~v) & 0xFFFFFFFF
            self._err(node, "not a constant")
        if node.kind == "binop":
            l = self._const(node.left, scope)
            r = self._const(node.right, scope)
            return self._const_binop(node, l, r)
        if node.kind == "ref" and node.args is not None and node.name in ("SIZE", "LENGTH", "LAST"):
            return self._const_builtin(node, scope)
        self._err(node, "constant expression required")

    def _const_binop(self, node, l, r):
        op = node.op
        if op == "+":
            return l + r
        if op == "-":
            return l - r
        if op == "*":
            return l * r
        if op == "/":
            if r == 0:
                self._err(node, "division by zero")
            return int(l / r) if False else (l // r if r else 0)
        if op == "MOD":
            if r == 0:
                self._err(node, "division by zero")
            return l % r
        if op == "AND":
            return l & r
        if op == "OR":
            return l | r
        if op == "XOR":
            return l ^ r
        if op == "PLUS":
            return (l + r) & 0xFFFFFFFF
        if op == "MINUS":
            return (l - r) & 0xFFFFFFFF
        self._err(node, "not a constant")

    def _const_builtin(self, node, scope):
        if len(node.args) != 1 or node.args[0].kind != "ref":
            self._err(node, f"{node.name} needs a variable")
        sym = scope.lookup(node.args[0].name)
        if sym is None or sym.ty is None:
            self._err(node, f"{node.args[0].name} is not declared")
        ty = self._type_of_ref(sym, node.args[0])
        if node.name == "SIZE":
            return ty.size
        if ty.kind != "array":
            self._err(node, f"{node.name} needs an array")
        if node.name == "LENGTH":
            return ty.length
        return ty.length - 1

    def _type_of_ref(self, sym, node):
        ty = sym.ty
        if node.args is not None:
            if ty.kind != "array":
                self._err(node, f"{sym.name} is not an array")
            if len(node.args) != 1:
                self._err(node, "arrays have one dimension")
            ty = ty.elem
        for fname in node.fields:
            if ty.kind != "struct":
                self._err(node, f"{fname} is not a structure field")
            match = None
            for f in ty.fields:
                if f["name"] == fname:
                    match = f
                    break
            if match is None:
                self._err(node, f"no field {fname}")
            ty = match["ty"]
        return ty

    # The bind path has to decorate parameters and finish inits. The first
    # _bind_vars tries scope.add and fails for parameters. Redo that path
    # by patching _bind_vars via a wrapper used above... it currently raises.
    # Fix: _bind_vars detects an existing parameter symbol.

    # ------------------------------------------------------- repair bind

    # (replaced below by the real methods if this class body is patched)

    def _label(self):
        self.nlabel += 1
        return f".L{self.nlabel}"

    def emit(self, text):
        self.lines.append(text)

    def _emit_header(self):
        name = self.module.name[:15]
        pad = 16 - len(name)
        self.emit("@ PL/M listing for a Freya flash program, ARM Cortex-M3.")
        self.emit("@ No floating point. ADDRESS is 32 bits.")
        self.emit("@ Link with boards/bluepill/app_flash.ld and run with runflash.")
        self.emit("@ RAM-loaded programs are not produced.")
        self.emit(f"@ Program flash 0x{FLASH_ADDR:08x}, {FLASH_SIZE} bytes.")
        self.emit("\t.syntax unified")
        self.emit("\t.cpu cortex-m3")
        self.emit("\t.arch armv7-m")
        self.emit("\t.thumb")
        self.emit("\t.section .app_header,\"a\",%progbits")
        self.emit("\t.global freya_header")
        self.emit("freya_header:")
        self.emit(f"\t.long 0x{APP_MAGIC:08x}")
        self.emit(f"\t.long {ABI_VERSION}")
        self.emit(f"\t.long 0x{FLASH_ADDR:08x}")
        self.emit("\t.long app_main")
        self.emit("\t.long __image_size__")
        self.emit("\t.long __bss_start__")
        self.emit("\t.long __bss_end__")
        self.emit(f"\t.long {int(self.stack)}")
        self.emit(f'\t.ascii "{name}"')
        if pad:
            self.emit(f"\t.space {pad}, 0")
        self.emit(f"\t.long {XIP_FLAG}")
        self.emit("\t.long __data_load__")
        self.emit("\t.long __data_start__")
        self.emit("\t.long __data_end__")
        self.emit("\t.long 0")
        self.emit("\t.long 0")

    def _emit_text_start(self):
        self.emit("\t.section .text,\"ax\",%progbits")
        self.emit("\t.thumb_func")
        self.emit("\t.type app_main, %function")
        self.emit("\t.global app_main")
        self.emit("app_main:")
        self.emit("\tpush {r4, lr}")
        self.emit("\tldr r4, =plm_api")
        self.emit("\tstr r0, [r4]")
        self.emit("\tldr r4, =plm_argc")
        self.emit("\tstr r1, [r4]")
        self.emit("\tldr r4, =plm_argv")
        self.emit("\tstr r2, [r4]")
        self.emit("\tbl plm_main")
        self.emit("\tpop {r4, pc}")
        self.emit("")

    def _emit_main(self):
        self._finish_scope_tree(self.scope)
        self.proc = None
        self.exit_label = self._label()
        self.emit("\t.thumb_func")
        self.emit("\t.type plm_main, %function")
        self.emit("plm_main:")
        self.emit("\tpush {r4, lr}")
        self._gen_stmts(self.module.stmts, self.scope)
        self.emit("\tmovs r0, #0")
        self.emit(f"{self.exit_label}:")
        self.emit("\tpop {r4, pc}")
        self.emit("\t.ltorg")
        self.emit("")

    def _finish_scope_tree(self, scope):
        # Resolve BASED and lay out initialisers now that every name in the
        # scope exists. Nested procedures were bound with their own scopes
        # stored on the proc symbol.
        self._fixup_scope(scope)
        for sym in scope.syms.values():
            if sym.skind == "proc" and not sym.external:
                self._finish_scope_tree(sym.inner)

    def _fixup_scope(self, scope):
        for sym in list(scope.syms.values()):
            if sym.skind != "var":
                continue
            if sym.base_name and sym.base is None:
                base = scope.lookup(sym.base_name)
                if base is None or not base.ty or base.ty.kind != "address":
                    raise PlmError(f"{sym.name} is BASED on a non-ADDRESS")
                sym.base = base
        for sym in list(scope.syms.values()):
            if sym.skind != "var" or not hasattr(sym, "_star"):
                continue
            init = sym._init_ast
            if sym._star and init is None:
                raise PlmError(f"{sym.name} needs an initialiser")
            if init is None:
                if sym.storage == "bss":
                    self._emit_space(sym)
                elif sym.storage == "extern":
                    self.bss.append(f"\t.extern {sym.asm}")
                continue
            blob = self._encode_init(sym, init, scope)
            if sym._star:
                elem = sym._elem
                if elem.size == 0 or len(blob) % elem.size:
                    raise PlmError(f"{sym.name} initialiser length is wrong")
                n = len(blob) // elem.size
                sym.ty = Ty("array", len(blob), elem.align, elem=elem, length=n)
            elif sym.ty.kind == "array":
                if len(blob) > sym.ty.size:
                    raise PlmError(f"{sym.name} initialiser is too long")
                blob = blob + bytes(sym.ty.size - len(blob))
            sym.init = blob
            if sym.const:
                sym.storage = "rodata"
                self._emit_bytes(self.rodata, sym, blob)
            else:
                sym.storage = "data"
                self._emit_bytes(self.data, sym, blob)

    def _emit_proc(self, sym):
        if sym.external:
            return
        node = sym.proc_node
        prev = self.proc
        prev_exit = self.exit_label
        self.proc = sym
        self.exit_label = self._label()
        self.emit("\t.thumb_func")
        self.emit(f"\t.type {sym.asm}, %function")
        if sym.public:
            self.emit(f"\t.global {sym.asm}")
        self.emit(f"{sym.asm}:")
        if sym.reentrant:
            self.emit("\tpush {r4, r7, lr}")
            self.emit("\tsub sp, #4")
            self.emit("\tmov r7, sp")
            if sym.frame_size:
                self.emit(f"\tsub sp, #{sym.frame_size}")
            for i, p in enumerate(sym.params):
                self.emit(f"\tstr r{i}, [r7, #-{p.frame_off}]")
        else:
            self.emit("\tpush {r4, lr}")
        self._gen_stmts(node.stmts, sym.inner)
        if sym.ret is None:
            self.emit("\tmovs r0, #0")
        self.emit(f"{self.exit_label}:")
        if sym.reentrant:
            self.emit("\tmov sp, r7")
            self.emit("\tadd sp, #4")
            self.emit("\tpop {r4, r7, pc}")
        else:
            self.emit("\tpop {r4, pc}")
        self.emit("\t.ltorg")
        self.emit("")
        for child in sym.inner.syms.values():
            if child.skind == "proc":
                self._emit_proc(child)
        self.proc = prev
        self.exit_label = prev_exit

    def _emit_api_wrappers(self):
        seen = set()

        def walk(scope):
            for sym in scope.syms.values():
                if sym.skind == "proc" and sym.external and sym.name in API:
                    if sym.name not in seen:
                        seen.add(sym.name)
                        self._wrapper(sym)
                if sym.skind == "proc" and not sym.external:
                    walk(sym.inner)

        walk(self.scope)

    def _wrapper(self, sym):
        off = API[sym.name][0]
        self.emit("\t.thumb_func")
        self.emit(f"\t.type {sym.asm}, %function")
        self.emit(f"{sym.asm}:")
        self.emit("\tpush {r4, lr}")
        self.emit("\tldr r4, =plm_api")
        self.emit("\tldr r4, [r4]")
        self.emit(f"\tldr r4, [r4, #{off}]")
        self.emit("\tblx r4")
        self.emit("\tpop {r4, pc}")
        self.emit("")

    def _emit_sections(self):
        self.emit("\t.section .rodata,\"a\",%progbits")
        self.lines.extend(self.rodata)
        self.emit("\t.section .data,\"aw\",%progbits")
        self.lines.extend(self.data)
        self.emit("\t.section .bss,\"aw\",%nobits")
        self.lines.extend(self.bss)

    # --------------------------------------------------------- statements

    def _gen_stmts(self, stmts, scope, collect=True):
        if collect:
            self.defined_labels = set()
            self._collect_labels(stmts, scope)
        for st in stmts:
            self._gen_stmt(st, scope)

    def _collect_labels(self, stmts, scope):
        for st in stmts:
            self._collect_one(st, scope)

    def _collect_one(self, st, scope):
        if st.kind == "label":
            if st.name in self.defined_labels:
                self._err(st, f"label {st.name} is repeated")
            self.defined_labels.add(st.name)
            found = scope.lookup(st.name)
            if found is None:
                found = Sym(st.name)
                found.skind = "label"
                scope.add(found)
            elif found.skind != "label":
                self._err(st, f"{st.name} is not a label")
            found.lab_asm = f".Llab_{id(scope)}_{st.name}"
            self._collect_one(st.stmt, scope)
        elif st.kind == "if":
            self._collect_one(st.then, scope)
            if st.els:
                self._collect_one(st.els, scope)
        elif st.kind in ("block", "while", "doloop"):
            self._collect_labels(st.body, scope)
        elif st.kind == "case":
            for arm in st.arms:
                self._collect_one(arm, scope)

    def _gen_stmt(self, st, scope):
        self.emit(f"\t@ line {st.line}")
        if st.kind == "empty":
            return
        if st.kind == "label":
            sym = scope.lookup(st.name)
            self.emit(f"{sym.lab_asm}:")
            self._gen_stmt(st.stmt, scope)
            return
        if st.kind == "assign":
            self._gen_assign(st, scope)
            return
        if st.kind == "call":
            self._gen_call_stmt(st.call, scope)
            return
        if st.kind == "return":
            if st.expr is not None:
                self._gen_value(st.expr, scope)
            else:
                self.emit("\tmovs r0, #0")
            self.emit(f"\tb {self.exit_label}")
            return
        if st.kind == "halt":
            self.emit("\tmovs r0, #0")
            # HALT ends the flash program and returns to the Freya shell.
            main_exit = ".L_halt_main"
            # Always leave through the outermost exit, which is plm_main's
            # when we are in the module, and the procedure exit otherwise.
            self.emit(f"\tb {self.exit_label}")
            return
        if st.kind == "goto":
            sym = scope.lookup(st.name)
            if sym is None or sym.skind != "label" or not getattr(sym, "lab_asm", None):
                self._err(st, f"label {st.name} is not declared")
            self.emit(f"\tb {sym.lab_asm}")
            return
        if st.kind == "if":
            self._gen_if(st, scope)
            return
        if st.kind == "block":
            self._gen_stmts(st.body, scope, collect=False)
            return
        if st.kind == "while":
            top = self._label()
            end = self._label()
            self.emit(f"{top}:")
            self._gen_value(st.cond, scope)
            self.emit("\ttst r0, #1")
            self.emit(f"\tbeq {end}")
            self._gen_stmts(st.body, scope, collect=False)
            self.emit(f"\tb {top}")
            self.emit(f"{end}:")
            return
        if st.kind == "case":
            self._gen_case(st, scope)
            return
        if st.kind == "doloop":
            self._gen_doloop(st, scope)
            return
        self._err(st, f"cannot compile {st.kind}")

    def _gen_if(self, st, scope):
        els = self._label()
        end = self._label()
        self._gen_value(st.cond, scope)
        self.emit("\ttst r0, #1")
        self.emit(f"\tbeq {els if st.els else end}")
        self._gen_stmt(st.then, scope)
        if st.els:
            self.emit(f"\tb {end}")
            self.emit(f"{els}:")
            self._gen_stmt(st.els, scope)
        self.emit(f"{end}:")

    def _gen_case(self, st, scope):
        end = self._label()
        labels = [self._label() for _ in st.arms]
        self._gen_value(st.expr, scope)
        for i, lab in enumerate(labels):
            self.emit(f"\tcmp r0, #{i}")
            self.emit(f"\tbeq {lab}")
        self.emit(f"\tb {end}")
        for lab, arm in zip(labels, st.arms):
            self.emit(f"{lab}:")
            self._gen_stmt(arm, scope)
            self.emit(f"\tb {end}")
        self.emit(f"{end}:")

    def _gen_doloop(self, st, scope):
        sym = scope.lookup(st.var)
        if sym is None or sym.skind != "var" or not sym.ty.is_int():
            self._err(st, f"{st.var} is not an integer variable")
        step_node = st.step if st.step is not None else _num(1)
        down = False
        try:
            step_v = self._const(step_node, scope)
            down = sym.ty.signed and step_v < 0
        except PlmError:
            step_v = None
        self._gen_value(st.start, scope)
        self._store_sym(sym)
        limit_tmp = self._tmp()
        step_tmp = self._tmp()
        self._gen_value(st.limit, scope)
        self._store_asm(limit_tmp, DWORD)
        self._gen_value(step_node, scope)
        self._store_asm(step_tmp, DWORD)
        top = self._label()
        end = self._label()
        self.emit(f"{top}:")
        self._load_sym(sym)
        self._push()
        self._load_asm(limit_tmp, DWORD)
        self._pop("r1")
        # r1 = i, r0 = limit. Leave the loop when i has passed limit.
        if sym.ty.signed:
            cond = "blt" if down else "bgt"
        else:
            cond = "bhi"
        self.emit("\tcmp r1, r0")
        self.emit(f"\t{cond} {end}")
        self._gen_stmts(st.body, scope, collect=False)
        self._load_sym(sym)
        self._push()
        self._load_asm(step_tmp, sym.ty)
        self._pop("r1")
        self.emit("\tadds r0, r1, r0")
        self._mask("r0", sym.ty)
        self._store_sym(sym)
        self.emit(f"\tb {top}")
        self.emit(f"{end}:")

    def _tmp(self):
        self.ntmp += 1
        name = f"plm_tmp_{self.ntmp}"
        self.bss.append("\t.balign 4")
        self.bss.append(f"{name}:\n\t.space 4")
        return name

    # ---------------------------------------------------------- expressions

    def _gen_assign(self, st, scope):
        ty = self._lvalue_type(st.dst, scope)
        if ty.is_agg():
            self._err(st, "an array or structure is not assigned as a whole")
        if self._is_const_dest(st.dst, scope):
            self._err(st, "DATA is constant")
        self._gen_addr(st.dst, scope)
        self._push()
        self._gen_value(st.src, scope)
        self._mask("r0", ty)
        self._pop("r1")
        self._store_at("r1", ty)

    def _is_const_dest(self, ref, scope):
        sym = scope.lookup(ref.name)
        return sym is not None and sym.const

    def _gen_call_stmt(self, ref, scope):
        if ref.name == "MOVE" and scope.lookup("MOVE") is None:
            self._gen_move(ref, scope)
            return
        if ref.name in ("INPUT", "OUTPUT") and scope.lookup(ref.name) is None:
            self._err(ref, f"{ref.name} is not available; this target has no port I/O")
        sym = scope.lookup(ref.name)
        if sym is None or sym.skind != "proc":
            self._err(ref, f"{ref.name} is not a procedure")
        if ref.fields:
            self._err(ref, "a procedure has no fields")
        self._call_proc(sym, ref.args or [], scope)

    def _gen_move(self, ref, scope):
        args = ref.args or []
        if len(args) != 3:
            self._err(ref, "MOVE(count, source, dest)")
        self._gen_value(args[0], scope)
        self._push()
        self._gen_value(args[1], scope)
        self._push()
        self._gen_value(args[2], scope)
        self.emit("\tmov r2, r0")
        self._pop("r1")
        self._pop("r0")
        top = self._label()
        end = self._label()
        self.emit("\tmovs r3, #0")
        self.emit(f"{top}:")
        self.emit("\tcmp r3, r0")
        self.emit(f"\tbhs {end}")
        self.emit("\tldrb r12, [r1, r3]")
        self.emit("\tstrb r12, [r2, r3]")
        self.emit("\tadds r3, r3, #1")
        self.emit(f"\tb {top}")
        self.emit(f"{end}:")

    def _call_proc(self, sym, args, scope):
        if len(args) != len(sym.params):
            raise PlmError(f"{sym.name} expects {len(sym.params)} arguments")
        if sym.reentrant:
            for arg, param in zip(args, sym.params):
                if param.by_ref:
                    self._gen_addr(arg, scope)
                else:
                    self._gen_value(arg, scope)
                    self._mask("r0", param.ty)
                self._push()
            for reg in range(len(args) - 1, -1, -1):
                self._pop(f"r{reg}")
            self.emit(f"\tbl {sym.asm}")
            return
        for arg, param in zip(args, sym.params):
            if param.by_ref:
                self._gen_addr(arg, scope)
            else:
                self._gen_value(arg, scope)
                self._mask("r0", param.ty)
            self.emit(f"\tldr r1, ={param.asm}")
            self.emit("\tstr r0, [r1]")
        self.emit(f"\tbl {sym.asm}")

    def _gen_value(self, node, scope):
        if node.kind == "num":
            self._imm("r0", node.value)
            return CONST
        if node.kind == "str":
            self.nstr += 1
            lab = f"plm_str_{self.nstr}"
            data = ",".join(str(ord(c)) for c in node.value) + ", 0"
            self.rodata.append(f"\t.balign 4")
            self.rodata.append(f"{lab}:")
            self.rodata.append(f"\t.byte {data}")
            self.emit(f"\tldr r0, ={lab}")
            return ADDRESS
        if node.kind == "addr":
            self._gen_addr(node.ref, scope)
            return ADDRESS
        if node.kind == "unop":
            ty = self._gen_value(node.expr, scope)
            if not ty.is_int():
                self._err(node, "unary operator needs an integer")
            if node.op == "+":
                return ty
            if node.op == "-":
                self.emit("\trsbs r0, r0, #0")
                self._mask("r0", ty)
                return ty
            if node.op == "NOT":
                self.emit("\tmvns r0, r0")
                self._mask("r0", ty)
                return ty
        if node.kind == "binop":
            return self._gen_binop(node, scope)
        if node.kind == "ref":
            return self._gen_ref_value(node, scope)
        self._err(node, "bad expression")

    def _gen_ref_value(self, node, scope):
        if node.args is not None and node.name in BUILTINS and scope.lookup(node.name) is None:
            return self._gen_builtin(node, scope)
        sym = scope.lookup(node.name)
        if sym is None:
            self._err(node, f"{node.name} is not declared")
        if sym.skind == "proc":
            self._call_proc(sym, node.args or [], scope)
            if sym.ret is None:
                self._err(node, f"{sym.name} does not return a value")
            return sym.ret
        ty = self._lvalue_type(node, scope)
        if ty.is_agg():
            self._err(node, f"{node.name} needs a subscript or a field")
        self._gen_addr(node, scope)
        self._load_at("r0", ty)
        return ty

    def _gen_builtin(self, node, scope):
        name = node.name
        args = node.args or []
        if name in ("SIZE", "LENGTH", "LAST"):
            v = self._const_builtin(node, scope)
            self._imm("r0", v)
            return DWORD
        if name in ("LOW", "HIGH", "DOUBLE") and len(args) == 1:
            ty = self._gen_value(args[0], scope)
            if name == "LOW":
                self.emit("\tuxtb r0, r0")
                return BYTE
            if name == "HIGH":
                self.emit("\tlsrs r0, r0, #8")
                self.emit("\tuxtb r0, r0")
                return BYTE
            return DWORD if ty.kind != "byte" else WORD
        if name in ("SHL", "SHR", "ROL", "ROR") and len(args) == 2:
            ty = self._gen_value(args[0], scope)
            self._push()
            self._gen_value(args[1], scope)
            self.emit("\tmov r1, r0")
            self._pop("r0")
            if name == "SHL":
                self.emit("\tlsls r0, r0, r1")
            elif name == "SHR":
                self.emit("\tlsrs r0, r0, r1")
            elif name == "ROR":
                self._rotate(ty, left=False)
            else:
                self._rotate(ty, left=True)
            self._mask("r0", ty)
            return ty
        self._err(node, f"bad use of {name}")

    def _rotate(self, ty, left):
        # r0 value, r1 count. Rotate inside the type width.
        width = ty.size * 8
        self.emit(f"\tands r1, r1, #{width - 1}")
        if width == 32 and not left:
            self.emit("\trors r0, r0, r1")
            return
        if width == 32 and left:
            self.emit("\trsbs r1, r1, #32")
            self.emit("\trors r0, r0, r1")
            return
        self.emit("\tmov r2, r0")
        if left:
            self.emit("\tlsls r0, r2, r1")
            self.emit(f"\trsbs r3, r1, #{width}")
            self.emit("\tlsrs r2, r2, r3")
        else:
            self.emit("\tlsrs r0, r2, r1")
            self.emit(f"\trsbs r3, r1, #{width}")
            self.emit("\tlsls r2, r2, r3")
        self.emit("\torrs r0, r0, r2")

    def _gen_binop(self, node, scope):
        lt = self._gen_value(node.left, scope)
        self._push()
        rt = self._gen_value(node.right, scope)
        self._pop("r1")
        op = node.op
        if op in ("<", "<=", ">", ">=", "<>", "="):
            self._cmp_rel(op, self._rel_signed(lt, rt))
            return BYTE
        ty, signed = self._promote(node, lt, rt)
        if op in ("PLUS", "MINUS"):
            ty, signed = ADDRESS, False
        if op in ("+", "PLUS"):
            self.emit("\tadds r0, r1, r0")
        elif op in ("-", "MINUS"):
            self.emit("\tsubs r0, r1, r0")
        elif op == "*":
            self.emit("\tmuls r0, r1, r0")
        elif op in ("/", "MOD"):
            self._divmod(signed, op == "MOD")
        elif op == "AND":
            self.emit("\tands r0, r1, r0")
        elif op == "OR":
            self.emit("\torrs r0, r1, r0")
        elif op == "XOR":
            self.emit("\teors r0, r1, r0")
        else:
            self._err(node, f"unknown operator {op}")
        self._mask("r0", ty)
        return ty

    def _same_signed(self, lt, rt):
        if lt.kind == "integer" and rt.kind == "integer":
            return True
        if lt.kind == "integer" and rt.kind in ("byte", "word"):
            return True
        if rt.kind == "integer" and lt.kind in ("byte", "word"):
            return True
        return False

    def _rel_signed(self, lt, rt):
        kinds = {lt.kind, rt.kind}
        if "address" in kinds or "dword" in kinds:
            return False
        if "integer" in kinds or kinds == {"const"}:
            return True
        return False

    def _promote(self, node, lt, rt):
        if not lt.is_int() or not rt.is_int():
            self._err(node, "arithmetic needs integers")
        if lt.kind == "const" and rt.kind == "const":
            return INTEGER, True
        if lt.kind == "const":
            return rt, rt.signed
        if rt.kind == "const":
            return lt, lt.signed
        if self._same_signed(lt, rt) and "dword" not in (lt.kind, rt.kind) and "address" not in (lt.kind, rt.kind):
            if lt.kind == "integer" or rt.kind == "integer":
                return INTEGER, True
        if lt.kind in ("dword", "address", "integer") or rt.kind in ("dword", "address", "integer"):
            return DWORD, False
        if lt.kind == "word" or rt.kind == "word":
            return WORD, False
        return BYTE, False

    def _cmp_rel(self, op, signed):
        if signed:
            table = {"<": "lt", "<=": "le", ">": "gt", ">=": "ge",
                     "=": "eq", "<>": "ne"}
        else:
            table = {"<": "lo", "<=": "ls", ">": "hi", ">=": "hs",
                     "=": "eq", "<>": "ne"}
        cc = table[op]
        inv = {"lt": "ge", "le": "gt", "gt": "le", "ge": "lt",
               "lo": "hs", "ls": "hi", "hi": "ls", "hs": "lo",
               "eq": "ne", "ne": "eq"}[cc]
        self.emit("\tcmp r1, r0")
        self.emit(f"\tite {cc}")
        self.emit(f"\tmov{cc} r0, #255")
        self.emit(f"\tmov{inv} r0, #0")

    def _divmod(self, signed, mod):
        """r1 dividend, r0 divisor. Result in r0. Divisor 0 yields 0."""
        ok = self._label()
        zero = self._label()
        self.emit("\tcbz r0, " + zero)
        if mod:
            self.emit("\tmov r2, r1")
            self.emit("\tmov r3, r0")
        op = "sdiv" if signed else "udiv"
        self.emit(f"\t{op} r0, r1, r0")
        if mod:
            self.emit("\tmuls r0, r3, r0")
            self.emit("\tsubs r0, r2, r0")
        self.emit(f"\tb {ok}")
        self.emit(f"{zero}:")
        self.emit("\tmovs r0, #0")
        self.emit(f"{ok}:")

    def _lvalue_type(self, node, scope):
        if node.kind != "ref":
            self._err(node, "not a variable")
        sym = scope.lookup(node.name)
        if sym is None or sym.skind not in ("var", "param"):
            self._err(node, f"{node.name} is not a variable")
        return self._type_of_ref(sym, node)

    def _gen_addr(self, node, scope):
        if node.kind != "ref":
            self._err(node, "not addressable")
        sym = scope.lookup(node.name)
        if sym is None or sym.skind not in ("var", "param"):
            self._err(node, f"{node.name} is not a variable")
        if sym.by_ref or (sym.skind == "param" and sym.ty.is_agg()):
            self._load_sym_word(sym)
        elif sym.storage == "based":
            self._load_sym_word(sym.base)
        elif sym.storage == "abs":
            self._imm("r0", sym.at_addr)
        elif sym.storage == "frame":
            self.emit(f"\tsub r0, r7, #{sym.frame_off}")
        else:
            self.emit(f"\tldr r0, ={sym.asm}")
        ty = sym.ty
        if node.args is not None:
            if ty.kind != "array" and not (sym.skind == "param" and sym.by_ref):
                # A by-ref parameter's ty is the aggregate, so subscript works.
                if ty.kind != "array":
                    self._err(node, f"{sym.name} is not an array")
            if len(node.args) != 1:
                self._err(node, "arrays have one dimension")
            elem = ty.elem
            self._push()
            self._gen_value(node.args[0], scope)
            if elem.size == 1:
                pass
            elif elem.size == 2:
                self.emit("\tlsls r0, r0, #1")
            elif elem.size == 4:
                self.emit("\tlsls r0, r0, #2")
            else:
                self._imm("r1", elem.size)
                self.emit("\tmuls r0, r1, r0")
            self._pop("r1")
            self.emit("\tadds r0, r1, r0")
            ty = elem
        for fname in node.fields:
            if ty.kind != "struct":
                self._err(node, f"{fname} is not a field")
            match = None
            for f in ty.fields:
                if f["name"] == fname:
                    match = f
                    break
            if match is None:
                self._err(node, f"no field {fname}")
            if match["off"]:
                if match["off"] <= 255:
                    self.emit(f"\tadds r0, r0, #{match['off']}")
                else:
                    self._push()
                    self._imm("r0", match["off"])
                    self._pop("r1")
                    self.emit("\tadds r0, r1, r0")
            ty = match["ty"]

    def _load_sym(self, sym):
        self._addr_sym(sym)
        self._load_at("r0", sym.ty)

    def _store_sym(self, sym):
        # value is in r0
        self._push()
        self._addr_sym(sym)
        self.emit("\tmov r1, r0")
        self._pop("r0")
        self._mask("r0", sym.ty)
        self._store_at("r1", sym.ty)

    def _load_sym_word(self, sym):
        """Load the 32-bit cell of a symbol (a pointer or a by-ref slot)."""
        if sym.storage == "frame":
            self.emit(f"\tldr r0, [r7, #-{sym.frame_off}]")
        elif sym.storage == "abs":
            self._imm("r0", sym.at_addr)
            self.emit("\tldr r0, [r0]")
        else:
            self.emit(f"\tldr r0, ={sym.asm}")
            self.emit("\tldr r0, [r0]")

    def _addr_sym(self, sym):
        if sym.storage == "frame":
            self.emit(f"\tsub r0, r7, #{sym.frame_off}")
        elif sym.storage == "abs":
            self._imm("r0", sym.at_addr)
        elif sym.storage == "based":
            self._load_sym_word(sym.base)
        else:
            self.emit(f"\tldr r0, ={sym.asm}")

    def _load_at(self, reg, ty):
        if ty.size == 1:
            self.emit(f"\tldrb {reg}, [{reg}]")
        elif ty.size == 2:
            self.emit(f"\tldrh {reg}, [{reg}]")
        else:
            self.emit(f"\tldr {reg}, [{reg}]")

    def _store_at(self, reg, ty):
        if ty.size == 1:
            self.emit(f"\tstrb r0, [{reg}]")
        elif ty.size == 2:
            self.emit(f"\tstrh r0, [{reg}]")
        else:
            self.emit(f"\tstr r0, [{reg}]")

    def _store_asm(self, name, ty):
        self._push()
        self.emit(f"\tldr r0, ={name}")
        self.emit("\tmov r1, r0")
        self._pop("r0")
        self._store_at("r1", ty)

    def _load_asm(self, name, ty):
        self.emit(f"\tldr r0, ={name}")
        self._load_at("r0", ty)

    def _mask(self, reg, ty):
        if ty.size == 1:
            self.emit(f"\tuxtb {reg}, {reg}")
        elif ty.size == 2:
            self.emit(f"\tuxth {reg}, {reg}")

    def _imm(self, reg, value):
        value &= 0xFFFFFFFF
        if value <= 255:
            self.emit(f"\tmovs {reg}, #{value}")
        elif value >= 0xFFFFFF00:
            neg = (0x100000000 - value) & 0xFFFFFFFF
            if neg <= 255:
                self.emit(f"\tmovs {reg}, #{neg}")
                self.emit(f"\trsbs {reg}, {reg}, #0")
                return
            self.emit(f"\tldr {reg}, ={value}")
        else:
            self.emit(f"\tldr {reg}, ={value}")

    def _push(self):
        self.emit("\tpush {r0, r1}")

    def _pop(self, reg):
        # Eight-byte pop keeps the stack aligned for calls.
        other = "r12" if reg != "r12" else "r3"
        regs = sorted([reg, other], key=lambda r: int(r[1:]) if r.startswith("r") and r[1:].isdigit() else 99)
        # r12 sorts wrong with the key above (12). Fix explicitly.
        pair = [reg, other]
        pair.sort(key=lambda r: {"r0": 0, "r1": 1, "r2": 2, "r3": 3, "r12": 12}[r])
        self.emit("\tpop {" + ", ".join(pair) + "}")


def _num(v):
    from plm.parser import Node
    return Node("num", value=v, line=0, col=0, filename="")
