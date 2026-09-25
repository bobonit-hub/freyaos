"""PL/M compiler for Freya flash programs on ARM Cortex-M3."""

from plm.error import PlmError
from plm.lexer import Lexer
from plm.parser import Parser
from plm.codegen import Codegen

__all__ = ["PlmError", "compile_source", "compile_file"]


def compile_source(text, filename="<stdin>", include_dirs=None, stack=2048):
    """Compile one PL/M module. Returns a GNU assembler listing."""
    lexer = Lexer(text, filename, include_dirs or [])
    parser = Parser(lexer)
    module = parser.parse_module()
    return Codegen(module, stack=stack).generate()


def compile_file(path, include_dirs=None, stack=2048):
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()
    return compile_source(text, path, include_dirs, stack)
