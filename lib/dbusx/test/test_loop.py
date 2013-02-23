#
# This file is part of python-dbusx. Python-dbusx is free software
# available under the terms of the MIT license. See the file "LICENSE" that
# was provided together with this source file for the licensing terms.
#
# Copyright (c) 2012-2013 the python-dbusx authors. See the file "AUTHORS"
# for a complete list.

from nose import SkipTest

import dbusx
from dbusx.test.test_connection import TestConnection
from dbusx.test.test_object import TestObject, TestWrappedObject


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
        # that is supported and should work.
        cls.Connection.loop_instance = cls.create_loop()
        super(UseLoop, cls).setup_class()


# tulip (=pure Python) event loop

def create_tulip_loop():
    try:
        import tulip
    except ImportError:
        raise SkipTest('this test requires "tulip"')
    return tulip.get_event_loop()

class TestConnectionWithTulipLoop(UseLoop, TestConnection):
    create_loop = staticmethod(create_tulip_loop)

class TestObjectWithTulipLoop(UseLoop, TestObject):
    create_loop = staticmethod(create_tulip_loop)

class TestWrappedObjectWithTulipLoop(UseLoop, TestWrappedObject):
    create_loop = staticmethod(create_tulip_loop)


# PyUV (libuv) loop

def create_pyuv_loop():
    try:
        import looping
    except ImportError:
        raise SkipTest('this test requires "looping"')
    if not hasattr(looping, 'PyUVEventLoop'):
        raise SkipTest('this test requires "pyuv"')
    return looping.PyUVEventLoop()

class TestConnectionWithPyUVLoop(UseLoop, TestConnection):
    create_loop = staticmethod(create_pyuv_loop)

class TestObjectWithPyUVLoop(UseLoop, TestObject):
    create_loop = staticmethod(create_pyuv_loop)

class TestWrappedObjectWithPyUVLoop(UseLoop, TestWrappedObject):
    create_loop = staticmethod(create_pyuv_loop)


# PySide (Qt) loop

def create_pyside_loop():
    try:
        import looping
    except ImportError:
        raise SkipTest('this test requires "looping"')
    if not hasattr(looping, 'PySideEventLoop'):
        raise SkipTest('this test requires "PySide"')
    return looping.PySideEventLoop()

class TestConnectionWithPySideLoop(UseLoop, TestConnection):
    create_loop = staticmethod(create_pyside_loop)

class TestObjectWithPySideLoop(UseLoop, TestObject):
    create_loop = staticmethod(create_pyside_loop)

class TestWrappedObjectWithPySIdeLoop(UseLoop, TestWrappedObject):
    create_loop = staticmethod(create_pyside_loop)
