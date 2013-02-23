#
# This file is part of python-dbusx. Python-dbusx is free software
# available under the terms of the MIT license. See the file "LICENSE" that
# was provided together with this source file for the licensing terms.
#
# Copyright (c) 2012-2013 the python-dbusx authors. See the file "AUTHORS"
# for a complete list.

import six
import math

import dbusx
from dbusx.test import UnitTest, assert_raises


def cmp_seq0(a, b):
    """Compare if a[0] and b[0] are both sequences containing
    the same elements."""
    if len(a) != 1 or len(b) != 1 or len(a[0]) != len(b[0]):
        return False
    for i in range(len(a[0])):
        if a[0][i] != b[0][i]:
            return False
    return True


class TestMessage(UnitTest):

    def test_type(self):
        def test(type):
            msg = dbusx.Message(type)
            assert msg.type == type
        test(dbusx.MESSAGE_TYPE_METHOD_CALL)
        test(dbusx.MESSAGE_TYPE_METHOD_RETURN)
        test(dbusx.MESSAGE_TYPE_ERROR)
        test(dbusx.MESSAGE_TYPE_SIGNAL)

    def test_illegal_type(self):
        def test(type):
            assert_raises(ValueError, dbusx.Message, type)
        test(-100)
        test(-1)
        test(dbusx.MESSAGE_TYPE_INVALID)
        test(dbusx.NUM_MESSAGE_TYPES)
        test(dbusx.NUM_MESSAGE_TYPES+1)
        test(dbusx.NUM_MESSAGE_TYPES+100)

    def test_flags(self):
        def test(name, value):
            msg = dbusx.Message(dbusx.MESSAGE_TYPE_METHOD_CALL)
            setattr(msg, name, value)
            assert getattr(msg, name) == value
            kwargs = { name: value }
            msg = dbusx.Message(dbusx.MESSAGE_TYPE_METHOD_CALL, **kwargs)
            assert getattr(msg, name) == value
        test('no_reply', False)
        test('no_reply', True)
        test('no_auto_start', False)
        test('no_auto_start', True)

    def _serial_test(self, name):
        def test(value):
            msg = dbusx.Message(dbusx.MESSAGE_TYPE_METHOD_CALL)
            setattr(msg, name, value)
            assert getattr(msg, name) == value
            kwargs = { name: value }
            msg = dbusx.Message(dbusx.MESSAGE_TYPE_METHOD_CALL, **kwargs)
            assert getattr(msg, name) == value
        test(1)
        test(100)
        test(0xffffffff)

    def _illegal_serial_test(self, name):
        def test(value):
            msg = dbusx.Message(dbusx.MESSAGE_TYPE_METHOD_CALL)
            assert_raises(ValueError, setattr, msg, name, value)
            kwargs = { name: value }
            assert_raises(ValueError, dbusx.Message,
                          dbusx.MESSAGE_TYPE_METHOD_CALL, **kwargs)
        test(-1)
        test(0)
        test(0x100000000)

    def test_serial(self):
        self._serial_test('serial')

    def test_illegal_serial(self):
        self._illegal_serial_test('serial')

    def test_reply_serial(self):
        self._serial_test('reply_serial')

    def test_illegal_reply_serial(self):
        self._illegal_serial_test('reply_serial')

    def test_path(self):
        def test(path):
            msg = dbusx.Message(dbusx.MESSAGE_TYPE_METHOD_CALL)
            msg.path = path
            assert msg.path == path
            msg = dbusx.Message(dbusx.MESSAGE_TYPE_METHOD_CALL, path=path)
            assert msg.path == path
        test('/')
        test('/foo')
        test('/foo/bar')
        test('/FOO')
        test('/123')
        test('/_')

    def test_illegal_path(self):
        def test(path):
            assert not dbusx.check_path(path)
            msg = dbusx.Message(dbusx.MESSAGE_TYPE_METHOD_CALL)
            assert_raises(ValueError, setattr, msg, 'path', path)
            assert_raises(ValueError, dbusx.Message,
                         dbusx.MESSAGE_TYPE_METHOD_CALL, path=path)
        test('')
        test('foo')
        test('/foo/')
        test('/foo//bar')
        test('/foo/bar!')
        test('//')

    def _interface_name_test(self, name):
        def test(value):
            msg = dbusx.Message(dbusx.MESSAGE_TYPE_METHOD_CALL)
            setattr(msg, name, value)
            assert getattr(msg, name) == value
            kwargs = { name: value }
            msg = dbusx.Message(dbusx.MESSAGE_TYPE_METHOD_CALL, **kwargs)
            assert getattr(msg, name) == value
        test('com.example')
        test('com.Example')
        test('com.example1')
        test('com.example_')
        value = 'com.%s' % ('x' * (dbusx.MAXIMUM_NAME_LENGTH - 4))
        test(value)

    def _illegal_interface_name_test(self, name):
        def test(value):
            assert not getattr(dbusx, 'check_%s' % name)(name)
            msg = dbusx.Message(dbusx.MESSAGE_TYPE_METHOD_CALL)
            assert_raises(ValueError, setattr, msg, name, value)
            kwargs = { name: value }
            assert_raises(ValueError, dbusx.Message,
                          dbusx.MESSAGE_TYPE_METHOD_CALL, **kwargs)
        test('com')
        test('.com')
        test('1com')
        test('!com')
        test('com.example.')
        test('com.example!')
        test('com.1example')
        test('com.!example')
        test('1com.example')
        test('!com.example')
        test('com..example')
        test('com.%s' % ('x' * (dbusx.MAXIMUM_NAME_LENGTH - 3)))
 
    def test_interface(self):
        self._interface_name_test('interface')

    def test_illegal_interface(self):
        self._illegal_interface_name_test('interface')
 
    def test_error_name(self):
        self._interface_name_test('error_name')

    def test_illegal_error_name(self):
        self._illegal_interface_name_test('error_name')

    def test_member(self):
        def test(member):
            msg = dbusx.Message(dbusx.MESSAGE_TYPE_METHOD_CALL)
            msg.member = member
            assert msg.member == member
            msg = dbusx.Message(dbusx.MESSAGE_TYPE_METHOD_CALL, member=member)
            assert msg.member == member
        test('foo')
        test('Foo')
        test('foo123')
        test('x' * dbusx.MAXIMUM_NAME_LENGTH)

    def test_illegal_member(self):
        def test(member):
            assert not dbusx.check_member(member)
            msg = dbusx.Message(dbusx.MESSAGE_TYPE_METHOD_CALL)
            assert_raises(ValueError, setattr, msg, 'member', member)
            assert_raises(ValueError, dbusx.Message,
                          dbusx.MESSAGE_TYPE_METHOD_CALL, member=member)
        test('')
        test('1foo')
        test('!foo')
        test('x' * (dbusx.MAXIMUM_NAME_LENGTH+1))

    def _bus_name_test(self, name):
        def test(value):
            msg = dbusx.Message(dbusx.MESSAGE_TYPE_METHOD_CALL)
            setattr(msg, name, value)
            assert getattr(msg, name) == value
            kwargs = { name: value }
            msg = dbusx.Message(dbusx.MESSAGE_TYPE_METHOD_CALL, **kwargs)
            assert getattr(msg, name) == value
        test(':1.1')
        test(':123.123')
        test(':foo.bar')
        test(':foo.bar1')
        test(':foo.bar-')
        test(':Foo.Bar')
        test('foo.bar')
        test('foo.bar1')
        test('foo.bar-')
        test('Foo.Bar')

    def _illegal_bus_name_test(self, name):
        def test(value):
            assert not dbusx.check_bus_name(name)
            msg = dbusx.Message(dbusx.MESSAGE_TYPE_METHOD_CALL)
            assert_raises(ValueError, setattr, msg, name, value)
            kwargs = { name: value }
            assert_raises(ValueError, dbusx.Message,
                          dbusx.MESSAGE_TYPE_METHOD_CALL, **kwargs)
        test('')
        test(':1')
        test(':foo.bar.')
        test(':foo..bar')
        test(':foo.bar!')
        test('foo')
        test('1foo.bar')
        test('foo.1bar')
        test('foo.bar.')
        test('foo..bar')
        test('foo.bar!')

    def test_sender(self):
        self._bus_name_test('sender')

    def test_illegal_sender(self):
        self._illegal_bus_name_test('sender')

    def test_destination(self):
        self._bus_name_test('destination')

    def test_illegal_destination(self):
        self._illegal_bus_name_test('destination')

    def test_signature(self):
        msg = dbusx.Message(dbusx.MESSAGE_TYPE_METHOD_CALL)
        msg.set_args('ii', (1, 2))
        assert msg.signature == 'ii'

    def _echo(self, sig, args):
        msg = dbusx.Message(dbusx.MESSAGE_TYPE_METHOD_CALL)
        # Since .args is not cached, accessing .args does a proper round-trip
        # from Python objects to libdbus back to Python objects.
        msg.set_args(sig, args)
        return msg

    def _arg_test(self, sig, args, cmpfunc=None):
        msg = dbusx.Message(dbusx.MESSAGE_TYPE_METHOD_CALL)
        msg.set_args(sig, args)
        assert msg.signature == sig
        if cmpfunc is None:
            assert msg.args == args
        else:
            assert cmpfunc(msg.args, args)

    def _illegal_arg_type_test(self, sig, args):
        msg = dbusx.Message(dbusx.MESSAGE_TYPE_METHOD_CALL)
        assert_raises(TypeError, msg.set_args, sig, args)

    def _illegal_arg_value_test(self, sig, args):
        msg = dbusx.Message(dbusx.MESSAGE_TYPE_METHOD_CALL)
        assert_raises(ValueError, msg.set_args, sig, args)

    def test_arg_byte(self):
        self._arg_test('y', (0,))
        self._arg_test('y', (10,))
        self._arg_test('y', (0xff,))

    def test_arg_byte_illegal_type(self):
        self._illegal_arg_type_test('y', (None,))
        self._illegal_arg_type_test('y', (1.0,))
        self._illegal_arg_type_test('y', ('foo',))
        self._illegal_arg_type_test('y', ((),))
        self._illegal_arg_type_test('y', ([],))
        self._illegal_arg_type_test('y', ({},))

    def test_arg_byte_out_of_range(self):
        self._illegal_arg_value_test('y', (-1,))
        self._illegal_arg_value_test('y', (0x100,))

    def test_arg_int16(self):
        self._arg_test('n', (-0x8000,))
        self._arg_test('n', (-10,))
        self._arg_test('n', (-0,))
        self._arg_test('n', (10,))
        self._arg_test('n', (0x7fff,))

    def test_arg_int16_illegal_type(self):
        self._illegal_arg_type_test('n', (None,))
        self._illegal_arg_type_test('n', (1.0,))
        self._illegal_arg_type_test('n', ('foo',))
        self._illegal_arg_type_test('n', ((),))
        self._illegal_arg_type_test('n', ([],))
        self._illegal_arg_type_test('n', ({},))

    def test_arg_int16_out_of_range(self):
        self._illegal_arg_value_test('n', (-0x8001,))
        self._illegal_arg_value_test('n', (0x8000,))

    def test_arg_uint16(self):
        self._arg_test('q', (0,))
        self._arg_test('q', (10,))
        self._arg_test('q', (0xffff,))

    def test_arg_uint16_illegal_type(self):
        self._illegal_arg_type_test('q', (None,))
        self._illegal_arg_type_test('q', (1.0,))
        self._illegal_arg_type_test('q', ('foo',))
        self._illegal_arg_type_test('q', ((),))
        self._illegal_arg_type_test('q', ([],))
        self._illegal_arg_type_test('q', ({},))

    def test_arg_uint16_out_of_range(self):
        self._illegal_arg_value_test('q', (-1,))
        self._illegal_arg_value_test('q', (0x10000,))

    def test_arg_int32(self):
        self._arg_test('i', (-0x80000000,))
        self._arg_test('i', (-10,))
        self._arg_test('i', (0,))
        self._arg_test('i', (10,))
        self._arg_test('i', (0x7fffffff,))

    def test_arg_int32_illegal_type(self):
        self._illegal_arg_type_test('i', (None,))
        self._illegal_arg_type_test('i', (1.0,))
        self._illegal_arg_type_test('i', ('foo',))
        self._illegal_arg_type_test('i', ((),))
        self._illegal_arg_type_test('i', ([],))
        self._illegal_arg_type_test('i', ({},))

    def test_arg_int32_out_of_range(self):
        self._illegal_arg_value_test('i', (-0x80000001,))
        self._illegal_arg_value_test('i', (0x80000000,))

    def test_arg_uint32(self):
        self._arg_test('u', (0,))
        self._arg_test('u', (10,))
        self._arg_test('u', (0xffffffff,))

    def test_arg_uint32_illegal_type(self):
        self._illegal_arg_type_test('u', (None,))
        self._illegal_arg_type_test('u', (1.0,))
        self._illegal_arg_type_test('u', ('foo',))
        self._illegal_arg_type_test('u', ((),))
        self._illegal_arg_type_test('u', ([],))
        self._illegal_arg_type_test('u', ({},))
 
    def test_arg_uint32_out_of_range(self):
        self._illegal_arg_value_test('u', (-1,))
        self._illegal_arg_value_test('u', (0x100000000,))

    def test_arg_int64(self):
        self._arg_test('x', (-0x8000000000000000,))
        self._arg_test('x', (-10,))
        self._arg_test('x', (0,))
        self._arg_test('x', (10,))
        self._arg_test('x', (0x7fffffffffffffff,))

    def test_arg_int64_illegal_type(self):
        self._illegal_arg_type_test('x', (None,))
        self._illegal_arg_type_test('x', (1.0,))
        self._illegal_arg_type_test('x', ('foo',))
        self._illegal_arg_type_test('x', ((),))
        self._illegal_arg_type_test('x', ([],))
        self._illegal_arg_type_test('x', ({},))
 
    def test_arg_int64_out_of_range(self):
        self._illegal_arg_value_test('x', (-0x8000000000000001,))
        self._illegal_arg_value_test('x', (0x8000000000000000,))

    def test_arg_uint64(self):
        self._arg_test('t', (0,))
        self._arg_test('t', (10,))
        self._arg_test('t', (0xffffffffffffffff,))

    def test_arg_uint64_illegal_type(self):
        self._illegal_arg_type_test('t', (None,))
        self._illegal_arg_type_test('t', (1.0,))
        self._illegal_arg_type_test('t', ('foo',))
        self._illegal_arg_type_test('t', ((),))
        self._illegal_arg_type_test('t', ([],))
        self._illegal_arg_type_test('t', ({},))
 
    def test_arg_uint64_out_of_range(self):
        self._illegal_arg_value_test('t', (-1,))
        self._illegal_arg_value_test('t', (0x10000000000000000,))

    def test_arg_boolean(self):
        def cmp_bool(a, b):
            return len(a) == 1 and len(b) == 1 and a[0] == bool(b[0])
        self._arg_test('b', (False,))
        self._arg_test('b', (True,))
        self._arg_test('b', (None,), cmp_bool)
        self._arg_test('b', (0,), cmp_bool)
        self._arg_test('b', ((),), cmp_bool)
        self._arg_test('b', ([],), cmp_bool)
        self._arg_test('b', ({},), cmp_bool)
        self._arg_test('b', (1,), cmp_bool)
        self._arg_test('b', (2,), cmp_bool)
        self._arg_test('b', ([1],), cmp_bool)
        self._arg_test('b', ((1,),), cmp_bool)
        self._arg_test('b', ({1:1},), cmp_bool)

    def test_arg_double(self):
        self._arg_test('d', (-1e100,))
        self._arg_test('d', (-1.0,))
        self._arg_test('d', (-1e-100,))
        self._arg_test('d', (0.0,))
        self._arg_test('d', (1e-100,))
        self._arg_test('d', (1.0,))
        self._arg_test('d', (1e100,))

    def test_arg_double_int(self):
        self._arg_test('d', (-100,))
        self._arg_test('d', (-1,))
        self._arg_test('d', (0,))
        self._arg_test('d', (1,))
        self._arg_test('d', (100,))

    def test_arg_double_illegal_type(self):
        self._illegal_arg_type_test('d', (None,))
        self._illegal_arg_type_test('d', ('foo',))
        self._illegal_arg_type_test('d', ((),))
        self._illegal_arg_type_test('d', ([],))
        self._illegal_arg_type_test('d', ({},))
 
    def test_arg_double_special(self):
        inf = 1e1000
        self._arg_test('d', (inf,))
        self._arg_test('d', (-inf,))
        self._arg_test('d', (1/inf,))
        self._arg_test('d', (1/-inf,))
        nan = inf/inf
        def cmp_nan(a, b):
            # note: nan != nan
            return len(a) == 1 and len(b) == 1 and \
                        math.isnan(a[0]) and math.isnan(b[0])
        self._arg_test('d', (nan,), cmp_nan)

    def test_arg_string(self):
        self._arg_test('s', ('',)) == ('',)
        self._arg_test('s', ('foo',)) == ('foo',)

    def test_arg_string_unicode(self):
        self._arg_test('s', (six.u('foo \u20ac'),)) == (six.u('foo \u20ac'),)

    def test_arg_string_illegal_type(self):
        self._illegal_arg_type_test('s', (None,))
        self._illegal_arg_type_test('s', (1,))
        self._illegal_arg_type_test('s', (1.0,))
        self._illegal_arg_type_test('s', ((),))
        self._illegal_arg_type_test('s', ([],))
        self._illegal_arg_type_test('s', ({},))
 
    def test_arg_object_path(self):
        self._arg_test('o', ('/foo/bar',)) == ('/foo/bar',)

    def test_arg_object_path_invalid_type(self):
        self._illegal_arg_type_test('o', (None,))
        self._illegal_arg_type_test('o', (1,))
        self._illegal_arg_type_test('o', (1.0,))
        self._illegal_arg_type_test('o', ((),))
        self._illegal_arg_type_test('o', ([],))
        self._illegal_arg_type_test('o', ({},))
 
    def test_arg_object_path_invalid_value(self):
        self._illegal_arg_value_test('o', ('foo',))
        self._illegal_arg_value_test('o', ('foo/bar',))
        self._illegal_arg_value_test('o', ('/foo/bar/',))
        self._illegal_arg_value_test('o', ('/foo//bar/',))
        self._illegal_arg_value_test('o', ('/foo bar/',))

    def test_arg_signature(self):
        self._arg_test('g', ('iii',))

    def test_arg_signature_invalid_type(self):
        self._illegal_arg_type_test('g', (None,))
        self._illegal_arg_type_test('g', (1,))
        self._illegal_arg_type_test('g', (1.0,))
        self._illegal_arg_type_test('g', ((),))
        self._illegal_arg_type_test('g', ([],))
        self._illegal_arg_type_test('g', ({},))

    def test_arg_signature_invalid_value(self):
        self._illegal_arg_value_test('*', (1,))
        self._illegal_arg_value_test('(i', (1,))
        self._illegal_arg_value_test('i' * 256, (1,)*256)
        def nested_tuple(d,v):
            if d == 0:
                return (v,)
            return (nested_tuple(d-1,v),)
        self._illegal_arg_value_test('('*33 + 'i' + ')'*33,
                      nested_tuple(33,1))
        self._illegal_arg_value_test('a'*33+'i', nested_tuple(33,1))

    def test_arg_variant(self):
        self._arg_test('v', (('i', 10),))
        self._arg_test('v', (('ai', [1,2,3]),))

    def test_arg_variant_list(self):
        self._arg_test('v', (['i', 10],), cmp_seq0)
        self._arg_test('v', (['ai', [1,2,3]],), cmp_seq0)

    def test_arg_variant_invalid_type(self):
        self._illegal_arg_type_test('v', (None,))
        self._illegal_arg_type_test('v', (1,))
        self._illegal_arg_type_test('v', (1.0,))
        self._illegal_arg_type_test('v', ({},))
        self._illegal_arg_type_test('v', ((1, 2),))

    def test_arg_variant_invalid_value(self):
        self._illegal_arg_value_test('v', ((1,),))
        self._illegal_arg_value_test('v', ((1, 2, 3),))
        self._illegal_arg_value_test('v', (('ii', (1,2)),))

    def test_arg_struct(self):
        self._arg_test('(i)', ((1,),))
        self._arg_test('(ii)', ((1, 2),))
        self._arg_test('(iii)', ((1, 2, 3),))
        self._arg_test('(((((i)))))', ((((((1,),),),),),))

    def test_arg_struct_invalid_type(self):
        self._illegal_arg_type_test('(i)', (None,))
        self._illegal_arg_type_test('(i)', (1,))
        self._illegal_arg_type_test('(i)', (1.0,))
        self._illegal_arg_type_test('(i)', ((None,),))
    
    def test_arg_struct_invalid_value(self):
        self._illegal_arg_value_test('(u)', ((-1,),))

    def test_arg_array(self):
        self._arg_test('ai', ([1],))
        self._arg_test('ai', ([1,2],))
        self._arg_test('ai', ([1,2,3],))
        self._arg_test('a(ii)', ([(1,2), (3,4)],))
        self._arg_test('av', ([('i',10),('s','foo')],))

    def test_arg_array_tuple(self):
        self._arg_test('ai', ((1, 2, 3),), cmp_seq0)

    def test_arg_array_invalid_type(self):
        self._illegal_arg_type_test('ai', (None,))
        self._illegal_arg_type_test('ai', (1,))
        self._illegal_arg_type_test('ai', (1.0,))
        self._illegal_arg_type_test('ai', ('foo',))
        self._illegal_arg_type_test('ai', ({},))
        self._illegal_arg_type_test('ai', ([None],))

    def test_arg_array_invalid_value(self):
        self._illegal_arg_value_test('au', ([1, -1],))

    def test_arg_dict(self):
        self._arg_test('a{ss}', ({'foo': 'bar'},))
        self._arg_test('a{ss}', ({'foo': 'bar', 'baz': 'qux'},))
        self._arg_test('a{si}', ({'foo': 10},))
        self._arg_test('a{ii}', ({1: 10},))

    def test_arg_dict_illegal_type(self):
        self._illegal_arg_type_test('a{ss}', (None,))
        self._illegal_arg_type_test('a{ss}', (1,))
        self._illegal_arg_type_test('a{ss}', (1.0,))
        self._illegal_arg_type_test('a{ss}', ('foo',))
        self._illegal_arg_type_test('a{ss}', ((),))
        self._illegal_arg_type_test('a{ss}', ([],))
        self._illegal_arg_type_test('a{ss}', ({1: 10},))

    def test_arg_byte_array(self):
        self._arg_test('ay', (six.b('foo'),))

    def test_arg_byte_array_invalid_type(self):
        self._illegal_arg_type_test('ay', ([1,2,3],))

    def test_arg_multi(self):
        self._arg_test('ii', (1, 2))
        self._arg_test('iii', (1, 2, 3))

    def test_arg_too_few(self):
        self._illegal_arg_type_test('ii', (1,))

    def test_arg_too_many(self):
        self._illegal_arg_type_test('ii', (1,2,3))

    def test_arg_illegal_signature(self):
        self._illegal_arg_value_test('(i', ((10,),))
        self._illegal_arg_value_test('(i}', ((10,),))
