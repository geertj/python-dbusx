#
# This file is part of python-dbusx. Python-dbusx is free software
# available under the terms of the MIT license. See the file "LICENSE" that
# was provided together with this source file for the licensing terms.
#
# Copyright (c) 2012-2013 the python-dbusx authors. See the file "AUTHORS"
# for a complete list.

from __future__ import print_function

import six
import time
import dbusx
import dbusx.test


class TestConnection(dbusx.test.UnitTest):
    """Test suite for D-BUS connection.
    
    These tests are all without event loop.
    """

    Connection = dbusx.Connection

    def test_create(self):
        conn = self.Connection(dbusx.BUS_SESSION)
        conn.close()

    def test_get(self):
        conn = self.Connection.get(dbusx.BUS_SESSION)

    def test_get_private(self):
        conn = self.Connection.get(dbusx.BUS_SESSION, shared=False)
        conn.close()

    def test_unique_name(self):
        conn = self.Connection.get(dbusx.BUS_SESSION)
        assert conn.unique_name.startswith(':')

    def test_shared(self):
        conn = self.Connection.get(dbusx.BUS_SESSION)
        conn2 = self.Connection.get(dbusx.BUS_SESSION)
        assert conn is conn2

    def test_send_with_reply(self):
        # Call the "ListNames" method on the message bus using the
        # low-level send_with_reply() / read_write_dispatch() interface.
        conn = self.Connection.get(dbusx.BUS_SESSION)
        msg = dbusx.Message(dbusx.MESSAGE_TYPE_METHOD_CALL,
                            destination=dbusx.SERVICE_DBUS, path=dbusx.PATH_DBUS,
                            interface=dbusx.INTERFACE_DBUS, member='ListNames')
        replies = []
        def callback(message):
            replies.append(message)
        conn.send_with_reply(msg, callback)
        end_time = time.time() + 5.0
        while True:
            secs = end_time - time.time()
            if secs < 0 or replies:
                break
            if conn.loop:
                if conn.dispatch_status == dbusx.DISPATCH_DATA_REMAINS:
                    conn.dispatch()
                else:
                    conn.loop.run_once(secs)
            else:
                conn.read_write_dispatch(secs)
        assert len(replies) == 1
        reply = replies[0]
        assert isinstance(reply, dbusx.MessageBase)
        assert reply.type == dbusx.MESSAGE_TYPE_METHOD_RETURN
        assert reply.signature == 'as'
        args = reply.args
        assert isinstance(args, tuple)
        assert len(args) == 1
        assert isinstance(args[0], list)
        assert len(args[0]) > 0
        for arg in args[0]:
            assert isinstance(arg, six.string_types)

    def test_call_method(self):
        # Call "ListNames", now via call_method()
        conn = self.Connection(dbusx.BUS_SESSION)
        reply = conn.call_method(dbusx.SERVICE_DBUS, dbusx.PATH_DBUS,
                                 dbusx.INTERFACE_DBUS, 'ListNames', timeout=5)
        assert isinstance(reply, dbusx.MessageBase)
        assert reply.type == dbusx.MESSAGE_TYPE_METHOD_RETURN
        assert reply.signature == 'as'
        args = reply.args
        assert isinstance(args, tuple)
        assert len(args) == 1
        assert isinstance(args[0], list)
        assert len(args[0]) > 0
        for arg in args[0]:
            assert isinstance(arg, six.string_types)

    def test_connect_to_signal(self):
        # Call "RequestName" to request a new name. This should raise
        # the signal "NameAcquired".
        conn = self.Connection(dbusx.BUS_SESSION)
        name = 'org.example.Foo'
        replies = []
        def callback(message):
            if message.type == dbusx.MESSAGE_TYPE_SIGNAL \
                        and message.member == 'NameAcquired' \
                        and message.signature == 's' \
                        and message.args == (name,):
                replies.append(message)
        conn.connect_to_signal(dbusx.SERVICE_DBUS, dbusx.PATH_DBUS,
                               dbusx.INTERFACE_DBUS, 'NameAcquired',
                               callback=callback)
        conn.call_method(dbusx.SERVICE_DBUS, dbusx.PATH_DBUS,
                         dbusx.INTERFACE_DBUS, 'RequestName', 'su', (name, 0))
        end_time = time.time() + 5.0
        while True:
            secs = end_time - time.time()
            if secs < 0 or replies:
                break
            if conn.loop:
                if conn.loop.dispatch_status == dbusx.DISPATCH_DATA_REMAINS:
                    conn.dispatch()
                else:
                    conn.loop.run_once(secs)
            else:
                conn.read_write_dispach(secs)
        assert len(replies) == 1
        reply = replies[0]
        assert reply.args == (name,)
