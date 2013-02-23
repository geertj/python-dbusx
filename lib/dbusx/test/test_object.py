#
# This file is part of python-dbusx. Python-dbusx is free software
# available under the terms of the MIT license. See the file "LICENSE" that
# was provided together with this source file for the licensing terms.
#
# Copyright (c) 2012-2013 the python-dbusx authors. See the file "AUTHORS"
# for a complete list.

from __future__ import print_function

import time
import dbusx
from dbusx.test import UnitTest, assert_raises

SERVICE_FOO = 'org.example.FooService'
PATH_FOO = '/foo'
IFACE_FOO = 'org.example.Foo'
IFACE_FOO2 = 'org.example.Foo2'


class FooService(dbusx.Object):

    @dbusx.Method(IFACE_FOO, args_in='s', args_out='b')
    def HasAttr(self, name):
        return hasattr(self, name)

    @dbusx.Method(IFACE_FOO, args_in='s', args_out='s')
    def EchoString(self, s):
        return s

    @dbusx.Method(IFACE_FOO2, name='EchoString', args_in='s', args_out='s')
    def EchoStringUpper(self, s):
        return s.upper()

    @dbusx.Method(IFACE_FOO)
    def EchoAnything(self, *args):
        self.reply(self.message.signature, args)

    @dbusx.Method(IFACE_FOO, args_in='ss')
    def EvalExpr(self, args, sig):
        self.method.args_out = sig
        return eval(args)

    @dbusx.Method(IFACE_FOO, args_in='s')
    def RaiseError(self, name):
        self.error(name)

    MySignal = dbusx.Signal(IFACE_FOO, args='s')

    @dbusx.Method(IFACE_FOO, args_in='s')
    def EmitSignal(self, s):
        self.MySignal.emit(s)


class TestObject(UnitTest):

    Connection = dbusx.Connection

    @classmethod
    def setup_class(cls):
        super(TestObject, cls).setup_class()
        cls.conn = cls.Connection(dbusx.BUS_SESSION)
        cls.obj = FooService()
        cls.conn.publish(cls.obj, PATH_FOO)
        cls.proxy = cls.conn.proxy(cls.conn.unique_name, PATH_FOO,
                                   interfaces=(IFACE_FOO, IFACE_FOO2))

    @classmethod
    def teardown_class(cls):
        super(TestObject, cls).teardown_class()
        cls.conn.close()

    def test_attributes(self):
        proxy = self.proxy
        assert proxy.HasAttr('message')
        assert proxy.HasAttr('error')
        assert proxy.HasAttr('reply')

    def test_method_call(self):
        proxy = self.proxy
        assert proxy.EchoString('foo') == 'foo'

    def test_async_method_call(self):
        replies = []
        def callback(msg):
            replies.append(msg)
        proxy = self.proxy
        proxy.EchoString('foo', callback=callback)
        end_time = time.time() + 5.0
        while True:
            secs = end_time - time.time()
            if secs < 0 or replies:
                break
            if self.conn.loop:
                if self.conn.dispatch_status == dbusx.DISPATCH_DATA_REMAINS:
                    self.conn.dispatch()
                else:
                    self.conn.loop.run_once(secs)
            else:
                self.conn.read_write_dispatch(secs)
        assert len(replies) == 1
        reply = replies[0]
        assert reply.type == dbusx.MESSAGE_TYPE_METHOD_RETURN
        assert reply.signature == 's'
        assert reply.args == ('foo',)

    def test_call_method_no_signatures(self):
        proxy = self.proxy
        assert proxy.EchoAnything() == None
        assert proxy.EchoAnything('foo', signature='s') == 'foo'
        assert proxy.EchoAnything('foo', 'bar', signature='ss') == ('foo', 'bar')

    def test_echo_multiple_interfaces(self):
        proxy = self.proxy
        assert proxy.EchoString('foo') == 'foo'
        assert proxy.EchoString('foo', interface=IFACE_FOO2) == 'FOO'
        interfaces = proxy.interfaces
        proxy.interfaces = []
        try:
            # with no search path, we may not guess
            assert_raises(dbusx.Error, proxy.EchoString, 'foo')
        finally:
            proxy.interfaces = interfaces

    def test_return_no_args(self):
        proxy = self.proxy
        assert proxy.EvalExpr('None', '') == None

    def test_return_one_arg(self):
        proxy = self.proxy
        assert proxy.EvalExpr('1', 'i') == 1
        assert proxy.EvalExpr('"foo"', 's') == 'foo'
        assert proxy.EvalExpr('("foo","bar")', '(ss)') == ('foo', 'bar')
        assert proxy.EvalExpr('[1,2,3]', 'ai') == [1,2,3]

    def test_return_multiple_args(self):
        proxy = self.proxy
        assert proxy.EvalExpr('(1,2)', 'ii') == (1, 2)
        assert proxy.EvalExpr('(1,2,3)', 'iii') == (1, 2, 3)
        assert proxy.EvalExpr('((1,2),(3,4))', '(ii)(ii)') == ((1,2), (3,4))

    def test_return_wrong_arg_count(self):
        proxy = self.proxy
        assert_raises(dbusx.Error, proxy.EvalExpr, '1', '')
        assert_raises(dbusx.Error, proxy.EvalExpr, '(1,)', '')
        assert_raises(dbusx.Error, proxy.EvalExpr, '(1,2)', '')
        assert_raises(dbusx.Error, proxy.EvalExpr, '(1,2)', 'i')
        assert_raises(dbusx.Error, proxy.EvalExpr, '1', 'ii')
        assert_raises(dbusx.Error, proxy.EvalExpr, '(1,2,3)', 'ii')

    def test_return_wrong_arg_type(self):
        proxy = self.proxy
        assert_raises(dbusx.Error, proxy.EvalExpr, '1', 's')
        assert_raises(dbusx.Error, proxy.EvalExpr, '"foo"', 'i')

    def test_RaiseError(self):
        proxy = self.proxy
        err = assert_raises(dbusx.Error, proxy.RaiseError, 'foo.bar')
        assert err.args[0] == 'foo.bar'

    def test_raise_invalid_error(self):
        proxy = self.proxy
        err = assert_raises(dbusx.Error, proxy.RaiseError, '1foo.bar')
        assert err.args[0] == 'org.freedesktop.DBus.Error.Failed'

    def test_signal(self):
        proxy = self.proxy
        replies = []
        def callback(value):
            replies.append(value)
        proxy.MySignal.connect(callback)
        proxy.EmitSignal('foo')
        end_time = time.time() + 5.0
        while True:
            secs = end_time - time.time()
            if secs < 0 or replies:
                break
            if self.conn.loop:
                if self.conn.dispatch_status == dbusx.DISPATCH_DATA_REMAINS:
                    self.conn.dispatch()
                else:
                    self.conn.loop.run_once(secs)
            else:
                self.conn.read_write_dispatch(secs)
        assert replies[0] == 'foo'


class WrappedFooService(object):

    @dbusx.Method(IFACE_FOO, args_in='s', args_out='s')
    def EchoString(self, s):
        return s

    @dbusx.Method(IFACE_FOO, args_in='s', args_out='b')
    def HasAttr(self, name):
        return hasattr(self, name)



class TestWrappedObject(UnitTest):

    Connection = dbusx.Connection

    @classmethod
    def setup_class(cls):
        super(TestWrappedObject, cls).setup_class()
        cls.conn = cls.Connection(dbusx.BUS_SESSION)
        cls.obj = WrappedFooService()
        cls.conn.publish(cls.obj, PATH_FOO)
        cls.proxy = cls.conn.proxy(cls.conn.unique_name, PATH_FOO)

    @classmethod
    def teardown_class(cls):
        super(TestWrappedObject, cls).teardown_class()
        cls.conn.close()

    def test_attributes(self):
        proxy = self.proxy
        assert not proxy.HasAttr('message')
        assert not proxy.HasAttr('error')
        assert not proxy.HasAttr('reply')

    def test_call_method(self):
        proxy = self.proxy
        assert proxy.EchoString('foo') == 'foo'
