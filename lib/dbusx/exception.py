#
# This file is part of python-dbusx. Python-dbusx is free software
# available under the terms of the MIT license. See the file "LICENSE" that
# was provided together with this source file for the licensing terms.
#
# Copyright (c) 2012 the python-dbusx authors. See the file "AUTHORS" for a
# complete list.

from dbusx._dbus import Error

class RemoteError(Error):
    """A remote error has occurred."""

class TimeoutError(Error):
    """A timeout has occurred."""

class NoReply(Error):
    """Exception raised in a method call handler to indicate that no
    reply should be issued by the dispatcher."""

class MethodReply(Error):
    """Exception raised in a method call handler to perform a non-local
    return."""
