#
# This file is part of python-dbusx. Python-dbusx is free software
# available under the terms of the MIT license. See the file "LICENSE" that
# was provided together with this source file for the licensing terms.
#
# Copyright (c) 2012 the python-dbusx authors. See the file "AUTHORS" for a
# complete list.

import logging


def etree_indent(node, level=0):
    """Indent an ElementTree so that it will pretty print."""
    ws = '\n' + level * '  '
    if len(node):
        node.text = ws + '  '
        node.tail = ws
        for child in node:
            etree_indent(child, level+1)
        node[-1].tail = ws
    elif level:
        node.tail = ws


class ContextLogger(logging.LoggerAdapter):
    """A LoggerAdapter that prepends a message with some context. It allows
    to get good informative log messages without having to write long and
    repetitive logging statements.

    The context is prepended as a "[context message here]" string to the
    actual message.
    """

    def __init__(self, logger, context=None, store=None):
        self.logger = logger
        self.store = store or self
        self.store.context = context

    def process(self, msg, kwargs):
        if self.logger.getEffectiveLevel() == logging.DEBUG:
            if self.store.context is not None:
                msg = '[%s] %s' % (self.store.context, msg)
        return msg, kwargs

    def setContext(self, context, store=None):
        if store is not None:
            self.store = store
        self.store.context = context


def setupLogging():
    """Initialize the logging subsystem."""
    logger = logging.getLogger(__name__.split('.')[0])
    # Avoid one-off "No handlers could be found for logger XXX" error messages
    # in case this library is used in an application that does not configure
    # logging.
    logger.addHandler(logging.NullHandler())


def getLogger(name, context=None):
    """Return a ContextLogger for *name* that adds the information in *context*
    to log messages."""
    logger = logging.getLogger(name)
    adapter = ContextLogger(logger, context)
    return adapter
