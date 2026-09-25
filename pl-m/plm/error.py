"""Errors reported by the PL/M compiler."""


class PlmError(Exception):
    def __init__(self, message, line=None, col=None, filename=None):
        self.message = message
        self.line = line
        self.col = col
        self.filename = filename
        where = ""
        if filename:
            where += str(filename)
        if line is not None:
            where += ":" + str(line)
            if col is not None:
                where += ":" + str(col)
        if where:
            super().__init__(f"{where}: {message}")
        else:
            super().__init__(message)
