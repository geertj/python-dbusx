#
# This file is part of python-dbusx. Python-dbusx is free software
# available under the terms of the MIT license. See the file "LICENSE" that
# was provided together with this source file for the licensing terms.
#
# Copyright (c) 2012 the python-dbusx authors. See the file "AUTHORS" for a
# complete list.

from __future__ import print_function

import dbusx
import dbusx.util
from xml.etree import ElementTree as etree


class Method(object):
    """A method that can be called on an Object.

    When this class is used as a decorator on a method, it will expose the
    method over D-BUS.
    """

    def __init__(self, interface, name=None, args_in=None, args_out=None,
                 method=None, instance=None):
        """Create a new method.

        The *interface* argument specifies the method's interface. The name
        argument specifies the method name. If no name is provided, the
        `__name__` of the callback will be used instead.  The *args_in* and
        *args_out* arguments describe the inbound and outbound arguments,
        respectively. They must both be valid D-BUS signatures.
        """
        self.interface = interface
        self.name = name
        self.args_in = args_in
        self.args_out = args_out
        self.method = method
        self.instance = instance

    def __str__(self):
        if self.name is None:
            return '<Undecorated Method on %s>' % self.interface
        elif self.instance is None:
            return '<Decorated Method %s:%s>' % (self.interface, self.name)
        else:
            return '<Bound Method %s:%s to %s>' % \
                    (self.interface, self.name, repr(self.instance))

    def __call__(self, *args, **kwargs):
        """Decorate or call."""
        # If this is not a bound method, then this will set the `method`
        # attribute to args[0]. This allows this class to be used as a
        # decorator as well a callable method.
        if self.instance is None:
            self.method = args[0]
            if self.name is None:
                self.name = self.method.__name__
            return self
        else:
            return self.method(self.instance, *args, **kwargs)

    def __get__(self, obj, type=None):
        # A descriptor that implements method-like binding behavior. A new
        # method is returned that is bound to the object that this method
        # is an attribute of. So for example::
        #
        #   class MyService(Object):
        #
        #       @Method(args_in='s, args_out='s)
        #       def mymethod(self, s):
        #           pass
        #
        # In this case, `MyService.mymethod` is a Method instance. Assume
        # `service` is an instance of `MyServer`. When `mymethod` is called
        # as `service.mymethod()`, the "." lookup operator will call the
        # descriptor and bind the `.instance` attribute of the method to
        # `service`. Subsquently, when the `()` operator calls
        # `Method.__call__`, that method will pass the instance to the first
        # argument of the function `mymethod`.
        return Method(self.interface, self.name, self.args_in, self.args_out,
                      self.method, instance=obj)


class Signal(object):
    """A signal that can be raised from an Object.

    It is not required to define signals that are raised. However, by defining
    them, dbusx can include them in introspection requests.
    """

    def __init__(self, interface, name=None, args=None, instance=None):
        self.interface = interface
        self.name = name
        self.args = args
        self.instance = instance

    def __get__(self, obj, typ=None):
        # Find out our name..
        if self.name is None:
            for name in typ.__dict__:
                if typ.__dict__[name] is self:
                    break
            else:
                raise AttributeError('could not bind name')
        else:
            name = self.name
        return Signal(self.interface, name, self.args, instance=obj)

    def emit(self, *args, **kwargs):
        if self.instance is None:
            raise TypeError('cannot emit unbound signal')
        destination = kwargs.pop('destination', None)
        message = dbusx.Message.signal(destination, self.instance.path,
                        self.interface, self.name, self.args, args)
        self.instance.connection.send(message)


class Object(object):
    """An object published on the D-BUS.

    Objects implement methods, and can send signals. Methods must be decorated
    using the ``@Method`` decorator to expose them on the D-BUS.

    Signals should be decorated with the ``@Signal`` decorator. This is not
    strictly necessary but allows dbusx to include them in introspection
    replies.
    """

    def __init__(self):
        self.connection = None
        self.wrapped = None
        self.logger = dbusx.util.getLogger('dbusx.Object')

    @classmethod
    def wrap(cls, instance):
        """Wrap an object instance.
        
        The wrapped object does not need to derive from :class:`dbusx.Object`.
        Wrapping an object allows you to expose an existing object hierarchy on
        D-BUS without changing it to inherit from :class:`dbusx.Object`.
        """
        self = cls()
        self.wrapped = instance
        return self

    def register(self, connection, path):
        """Register an object with a connection. This is done automatically by
        a connection when an object is published.
        """
        self.connection = connection
        self.path = path

    def methods(self):
        """Iterate over all methods."""
        if self.wrapped:
            for sym in dir(self.wrapped):
                method = getattr(self.wrapped, sym)
                if isinstance(method, Method):
                    yield method
        for sym in dir(self):
            method = getattr(self, sym)
            if isinstance(method, Method):
                yield method

    def signals(self):
        """Iterate over all signals."""
        if self.wrapped:
            for sym in dir(self.wrapped):
                signal = getattr(self.wrapped, sym)
                if isinstance(signal, Signal):
                    yield signal
        for sym in dir(self):
            signal = getattr(self, sym)
            if isinstance(signal, Signal):
                yield signal

    def _process(self, connection, message):
        """Callback to process incoming messages."""
        if connection is not self.connection:
            return
        assert message.type == dbusx.MESSAGE_TYPE_METHOD_CALL
        for method in self.methods():
            if method.name != message.member or \
                        message.interface and \
                        method.interface != message.interface:
                continue
            self.connection._spawn(self._dispatch, method, message)
            return True
        return False

    def _dispatch(self, method, message):
        """Dispatch a method call."""
        log = self.logger
        context = 'methodcall %s:%s.%s' % \
                        (message.path, method.interface, method.name)
        log.setContext(context, store=self.connection.local)
        if method.args_in is not None and method.args_in != message.signature:
            log.error('invalid call signature (got: %s, expecting: %s)',
                      repr(message.signature), repr(method.args_in))
            self._error(dbusx.ERROR_INVALID_ARGS)
            return
        self.method = method
        self.message = message
        try:
            result = method(*message.args)
        except dbusx.NoReply:
            return
        except dbusx.MethodReply as e:
            result = e.args
        except dbusx.Error as e:
            error = str(e)
            if not dbusx.check_error_name(error):
                log.error('handler raised invalid error name: %s', repr(error))
                error = dbusx.ERROR_FAILED
            self._error(error)
            return
        except Exception as e:
            log.error('uncaught exception in handler', exc_info=True)
            self._error(dbusx.ERROR_FAILED)
            return
        signature = method.args_out or ''
        nargs = len(dbusx.split_signature(signature))
        if nargs == 0:
            if result is not None:
                log.error('handler should return None for signature %s ',
                          '(got %s instead)', repr(signature), repr(result))
                self._error(dbusx.ERROR_FAILED)
            result = ()
        elif nargs == 1:
            result = (result,)
        else:
            if not isinstance(result, tuple):
                log.error('handler should return tuple for signature %s ',
                          '(got %s instead)', repr(signature), repr(result))
                self._error(dbusx.ERROR_FAILED)
        argtest = dbusx.Message(dbusx.MESSAGE_TYPE_METHOD_RETURN)
        try:
            argtest.set_args(signature, result)
        except (TypeError, ValueError):
            log.error('handler return value does not match signature %s '
                      '(got: %s)', repr(signature), repr(result))
            self._error(dbusx.ERROR_FAILED)
            return
        self._reply(signature, result)

    def _error(self, error_name):
        reply = dbusx.Message(dbusx.MESSAGE_TYPE_ERROR, error_name=error_name,
                              reply_serial=self.message.serial,
                              destination=self.message.sender)
        self.connection.send(reply)

    def error(self, error_name):
        self._error(error_name)
        raise dbusx.NoReply()  # reply already sent

    def _reply(self, signature=None, args=None):
        reply = dbusx.Message(dbusx.MESSAGE_TYPE_METHOD_RETURN,
                              reply_serial=self.message.serial,
                              destination=self.message.sender)
        if signature is not None:
            reply.set_args(signature, args)
        self.connection.send(reply)

    def reply(self, signature=None, args=None):
        self._reply(signature, args)
        raise dbusx.NoReply()  # reply already sent

    def _get_method(self):
        return getattr(self.connection.local, 'method', None)

    def _set_method(self, method):
        self.connection.local.method = method

    method = property(_get_method, _set_method)

    def _get_message(self):
        return getattr(self.connection.local, 'message', None)

    def _set_message(self, message):
        self.connection.local.message = message

    message = property(_get_message, _set_message)

    @Method(interface=dbusx.INTERFACE_INTROSPECTABLE, args_out='s')
    def Introspect(self):
        path = self.message.path
        doc = etree.Element('node', name=path)
        interfaces = {}
        for method in self.methods():
            try:
                interfaces[method.interface][0].append(method)
            except KeyError:
                interfaces[method.interface] = ([method], [])
        for signal in self.signals():
            try:
                interfaces[signal.interface][1].append(signal)
            except KeyError:
                interfaces[signal.interface] = ([], [signal])
        for iface in interfaces:
            ifnode = etree.SubElement(doc, 'interface', name=iface)
            for method in interfaces[iface][0]:
                mnode = etree.SubElement(ifnode, 'method', name=method.name)
                if method.args_in is not None:
                    for ix,arg in enumerate(dbusx.split_signature(method.args_in)):
                        anode = etree.SubElement(mnode, 'arg', name='in%d' % ix,
                                           type=arg, direction='in')
                if method.args_out is not None:
                    for ix,arg in enumerate(dbusx.split_signature(method.args_out)):
                        anode = etree.SubElement(mnode, 'arg', name='out%d' % ix,
                                           type=arg, direction='out')
            for signal in interfaces[iface][1]:
                snode = etree.SubElement(ifnode, 'signal', name=signal.name)
                if signal.args is not None:
                    for ix,arg in enumerate(dbusx.split_signature(signal.args)):
                        anode = etree.SubElement(snode, 'arg', name='arg%d' % ix,
                                                 type=arg)
        dbusx.util.etree_indent(doc)
        xml = dbusx.INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE.upper()
        # can etree output unicode directly?
        xml += etree.tostring(doc, encoding='utf-8').decode('utf-8')
        return xml
