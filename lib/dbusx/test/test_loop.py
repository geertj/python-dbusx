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

    Connection = ConnectionWithLoop

    @staticmethod
    def create_loop(self):
        raise NotImplementedError

    @classmethod
    def setup_class(cls):
        # On purpose use the same loop instance for all tests in a single
        # suite, even if it creates multiple connections. This is something
        # that is supported.
        cls.Connection.loop_instance = cls.create_loop()
        super(UseLoop, cls).setup_class()


# tulip (=pure Python) event loop

def create_tulip_loop():
    try:
        import tulip
    except ImportError:
        raise dbusx.test.SkipTest('this test requires "tulip"')
    return tulip.get_event_loop()

class TestConnectionWithTulipLoop(UseLoop, test_connection.TestConnection):
    create_loop = staticmethod(create_tulip_loop)

class TestObjectWithTulipLoop(UseLoop, test_object.TestObject):
    create_loop = staticmethod(create_tulip_loop)

class TestWrappedObjectWithTulipLoop(UseLoop, test_object.TestWrappedObject):
    create_loop = staticmethod(create_tulip_loop)


# PyEV (libev) loop

def create_pyev_loop():
    try:
        import looping.pyev
    except ImportError:
        raise dbusx.test.SkipTest('this test requires "pyev"')
    return looping.pyev.EventLoop()

class TestConnectionWithPyEVLoop(UseLoop, test_connection.TestConnection):
    create_loop = staticmethod(create_pyev_loop)

class TestObjectWithPyEVLoop(UseLoop, test_object.TestObject):
    create_loop = staticmethod(create_pyev_loop)

class TestWrappedObjectWithPyEVLoop(UseLoop, test_object.TestWrappedObject):
    create_loop = staticmethod(create_pyev_loop)


# PyUV (libuv) loop

def create_pyuv_loop():
    try:
        import looping.pyuv
    except ImportError:
        raise dbusx.test.SkipTest('this test requires "pyuv"')
    return looping.pyuv.EventLoop()

class TestConnectionWithPyUVLoop(UseLoop, test_connection.TestConnection):
    create_loop = staticmethod(create_pyuv_loop)

class TestObjectWithPyUVLoop(UseLoop, test_object.TestObject):
    create_loop = staticmethod(create_pyuv_loop)

class TestWrappedObjectWithPyUVLoop(UseLoop, test_object.TestWrappedObject):
    create_loop = staticmethod(create_pyuv_loop)


# PySide (Qt) loop

#def create_pyside_loop():
#    try:
#        import looping.pyside
#    except ImportError:
#        raise dbusx.test.SkipTest('this test requires "PySide"')
#    return looping.pyside.EventLoop()

#class TestConnectionWithPySideLoop(UseLoop, test_connection.TestConnection):
#    create_loop = staticmethod(create_pyside_loop)

#class TestObjectWithPySideLoop(UseLoop, test_object.TestObject):
#    create_loop = staticmethod(create_pyside_loop)

#class TestWrappedObjectWithPySIdeLoop(UseLoop, test_object.TestWrappedObject):
#    create_loop = staticmethod(create_pyside_loop)
