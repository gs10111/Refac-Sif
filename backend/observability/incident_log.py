"""Carries the failures the server logs to the page the operator is looking at.

The server already logs every failure it hits — thirteen call sites when this was
written, across the TCP path, the CSV, the Drive copy and the publisher. All of
them went to a terminal nobody watches, so an operator on the web page saw a
healthy-looking screen while captures were being refused.

This is a logging handler rather than a call at each of those sites: nothing has
to be instrumented twice, and a failure added later is visible without anyone
remembering to record it.
"""
import logging

from app_state import AppState, INCIDENT_LEVELS

# Set on the handler so a second install can find the first one instead of
# stacking, which would show every failure twice and halve the ring's depth.
_HANDLER_MARK = '_sif_incident_log'

# Loggers whose warnings are about serving a page, not about the fleet. Werkzeug
# is in this process and talks about HTTP requests; letting it in would push real
# failures out of a ring the operator reads twenty lines of.
IGNORED_LOGGERS = ('werkzeug',)


class IncidentLogHandler(logging.Handler):
    """Pushes WARNING and worse into the state the page renders."""

    def __init__(self, state: AppState):
        super().__init__(level=logging.WARNING)
        self._state = state
        setattr(self, _HANDLER_MARK, True)

    def emit(self, record: logging.LogRecord) -> None:
        try:
            if record.levelname not in INCIDENT_LEVELS:
                return
            if record.name.split('.')[0] in IGNORED_LOGGERS:
                return
            # record.getMessage() applies the % arguments logging defers, and
            # deliberately leaves the traceback out: the operator needs the
            # sentence, and a stack in a table cell hides the failures around it.
            self._state.record_incident(record.levelname, record.getMessage())
        except Exception:                      # pragma: no cover - handler contract
            # A logging handler that raises takes down the call that logged, which
            # here is already an error path. Nothing is worth that.
            self.handleError(record)


def install_incident_log(state: AppState, logger: logging.Logger | None = None
                         ) -> IncidentLogHandler:
    """Attach the handler to the root logger, once.

    Returns the handler in place — the existing one if it is already installed —
    so a caller can remove exactly what it added.
    """
    logger = logger or logging.getLogger()
    for handler in logger.handlers:
        if getattr(handler, _HANDLER_MARK, False):
            return handler

    handler = IncidentLogHandler(state)
    logger.addHandler(handler)
    return handler


def uninstall_incident_log(handler: IncidentLogHandler,
                           logger: logging.Logger | None = None) -> None:
    """Detach a handler installed above. Used by tests; harmless if absent."""
    logger = logger or logging.getLogger()
    if handler in logger.handlers:
        logger.removeHandler(handler)
