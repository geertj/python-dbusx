#
# This file is part of python-dbusx. Python-dbusx is free software
# available under the terms of the MIT license. See the file "LICENSE" that
# was provided together with this source file for the licensing terms.
#
# Copyright (c) 2012 the python-dbusx authors. See the file "AUTHORS" for a
# complete list.

import dbusx
import dbusx.test
from dbusx.test import test_connection, test_object

import looping


# Mixin to use a specific loop

class ConnectionWithLoop(dbusx.Connection):

    loop_instance = None

    def __init__(self, address):
        super(ConnectionWithLoop, self).__init__(address)
        self.set_loop(self.loop_instance)

    @classmethod
    def get(cls, address, shared=True):
        self = super(ConnectionWithLoop, cls).get(address, shared)
        if self.loop is None:
            self.set_loop(self.loop_instance)
        return self


class UseLoop(object):

    loop_module = None
    Connection = ConnectionWithLoop

    @classmethod
    def setup_class(cls):
        parts = cls.loop_module.split('.')
        base = '.'.join(parts[:-1])
        name = parts[-1]
        mod = __import__(base, globals(), locals(), [name], -1)
        mod = getattr(mod, name)
        if not mod.available:
            raise dbusx.test.SkipTest('Skipping %s tests (%s not installed)'
                                      % (cls.__name__, name))
        # On purpose use the same loop instance for all tests in a single
        # suite, even if it creates multiple connections. This is something
        # that is supported.
        cls.Connection.loop_instance = mod.EventLoop()
        super(UseLoop, cls).setup_class()


# tulip loop

class TestConnectionWithTulipLoop(UseLoop, test_connection.TestConnection):
    loop_module = 'looping.tulip'

class TestObjectWithTulipLoop(UseLoop, test_object.TestObject):
    loop_module = 'looping.tulip'

class TestWrappedObjectWithTulipLoop(UseLoop, test_object.TestWrappedObject):
    loop_module = 'looping.tulip'


# PyUV (libuv) loop

class TestConnectionWithPyUVLoop(UseLoop, test_connection.TestConnection):
    loop_module = 'looping.pyuv'

class TestObjectWithPyUVLoop(UseLoop, test_object.TestObject):
    loop_module = 'looping.pyuv'

class TestWrappedObjectWithPyUVLoop(UseLoop, test_object.TestWrappedObject):
    loop_module = 'looping.pyuv'


# PyEV (libev) loop

class TestConnectionWithPyEVLoop(UseLoop, test_connection.TestConnection):
    loop_module = 'looping.pyev'

class TestObjectWithPyEVLoop(UseLoop, test_object.TestObject):
    loop_module = 'looping.pyev'

class TestWrappedObjectWithPyEVLoop(UseLoop, test_object.TestWrappedObject):
    loop_module = 'looping.pyev'


# PySide (Qt) loop

try:
    from PySide import QtCore
except ImportError:
    pass
else:
    import sys
    qapp = QtCore.QCoreApplication(sys.argv)


class TestConnectionWithPySideLoop(UseLoop, test_connection.TestConnection):
    loop_module = 'looping.pyside'

class TestObjectWithPySideLoop(UseLoop, test_object.TestObject):
    loop_module = 'looping.pyside'

class TestWrappedObjectWithPySideLoop(UseLoop, test_object.TestWrappedObject):
    loop_module = 'looping.pyside'
