"""Persistent logging shared by the local console and the web service.

Everything the engine prints (HDMI capture state, worker errors), every uncaught exception
in a thread and every error the interfaces report goes to one rotating log file so a
fault that flashed by on screen can still be read afterwards:

    <data>/logs/mechanical-tv.log      (1 MB x 5 files)

The console also receives the same records through a queue for its debug window.
"""
import logging
import logging.handlers
from pathlib import Path
import sys
import threading
import traceback

LOGGER = logging.getLogger("mechanical_tv")
LOG_NAME = "mechanical-tv.log"
FORMAT = "%(asctime)s %(levelname)-7s %(threadName)s: %(message)s"


class _Tee:
    """A stdout/stderr replacement: still writes to the original stream, and logs each full line."""

    def __init__(self, stream, level):
        self.stream = stream
        self.level = level
        self._buffer = ""
        self._lock = threading.Lock()

    def write(self, text):
        if not isinstance(text, str):
            text = str(text)
        try:
            self.stream.write(text)
        except (OSError, ValueError):
            pass  # journald/terminal gone; the log file still gets the line
        with self._lock:
            self._buffer += text
            lines = self._buffer.split("\n")
            self._buffer = lines.pop()
        for line in lines:
            if line.strip():
                LOGGER.log(self.level, line.rstrip())
        return len(text)

    def flush(self):
        try:
            self.stream.flush()
        except (OSError, ValueError):
            pass

    def isatty(self):
        return False

    def fileno(self):
        return self.stream.fileno()

    @property
    def encoding(self):
        return getattr(self.stream, "encoding", "utf-8")


def _thread_hook(previous):
    def hook(args):
        LOGGER.error("Uncaught exception in thread %s: %s", getattr(args.thread, "name", "?"),
                     "".join(traceback.format_exception(args.exc_type, args.exc_value, args.exc_traceback)).rstrip())
        if previous is not None:
            previous(args)
    return hook


def setup_logging(directory, capture_std=True, listener=None):
    """Log to <directory>/mechanical-tv.log; optionally mirror print() and feed a queue for a UI.

    Returns the log file path. Safe to call more than once (handlers are added only once).
    """
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / LOG_NAME
    LOGGER.setLevel(logging.DEBUG)
    LOGGER.propagate = False
    if not any(isinstance(h, logging.handlers.RotatingFileHandler) for h in LOGGER.handlers):
        handler = logging.handlers.RotatingFileHandler(path, maxBytes=1_000_000, backupCount=5, encoding="utf-8")
        handler.setFormatter(logging.Formatter(FORMAT))
        LOGGER.addHandler(handler)
    if listener is not None and not any(isinstance(h, logging.handlers.QueueHandler) for h in LOGGER.handlers):
        LOGGER.addHandler(logging.handlers.QueueHandler(listener))
    if capture_std:
        if not isinstance(sys.stdout, _Tee):
            sys.stdout = _Tee(sys.stdout, logging.INFO)
        if not isinstance(sys.stderr, _Tee):
            sys.stderr = _Tee(sys.stderr, logging.ERROR)
    if not getattr(threading.excepthook, "_mtv_hook", False):
        hook = _thread_hook(threading.excepthook)
        hook._mtv_hook = True
        threading.excepthook = hook
    return path


def log_exception(where, exc):
    """Record an exception with its traceback (used by UI action handlers)."""
    LOGGER.error("%s: %s: %s\n%s", where, type(exc).__name__, exc,
                 "".join(traceback.format_exception(type(exc), exc, exc.__traceback__)).rstrip())
