#
# This file is part of python-dbusx. Python-dbusx is free software
# available under the terms of the MIT license. See the file "LICENSE" that
# was provided together with this source file for the licensing terms.
#
# Copyright (c) 2012 the python-dbusx authors. See the file "AUTHORS" for a
# complete list.

from __future__ import print_function

import six
import dbusx
import dbusx.util
from xml.etree import ElementTree as etree


class MethodStub(object):
    """Method call stub.

    Instances of this class are callable, and added as attributes to a Proxy
    instance. Calling the method on a proxy will call the remote D-BUS method
    and return its result.

    The calling convention has been designed to be as similar as possible to
    calling a regular Python method. Howver there are a few things to note that
    are different and need to be noted. The calling convention is defined as
    follows:

     * D-BUS methods always takes positional arguments, never keyword arguments.
     * D-BUS objects can have multiple interfaces, while Python objects only
       have one. It is possible that multiple interfaces on the same D-BUS
       object define a method with the same name. In such a case, the interface
       to use needs to be resolved. This is done using the interface search
       path on the Proxy, or by explicitly specifying it with the *interface*
       keyword argument when calling the method. Python-dbusx will never guess
       the interface. If it is not clear by the search path or by explicit
       specification which method is supposed to be called, an exception is
       raised.
     * D-BUS methods can return zero, one or multiple values, while Python
       methods always one value. In case zero values are returned, this is
       translated to a Python `None`. In case one value is returned, the value
       is returned as-is. In case multiple values are return, the values are
       returned in a tuple.
     * When a D-BUS method returns an error, a :class:`Error` exception is
       raised with the error name as its message.

    Method calls are normally synchronous, but asynchronous method calls can be
    requested by using a *callback* keyword parameter. This requires that an
    event loop is present.
    """

    def __init__(self, proxy, interface, method, signature):
        """Create a new MethodStub instance."""
        self.proxy = proxy
        self.method = method
        self.interfaces = [(interface, signature)]

    def add_interface(self, interface, signature):
        """Add another interface for this method."""
        self.interfaces.append((interface, signature))

    def _resolve_interface(self):
        """If there are multple interfaces for this method, resolve the
        interface using the interface search path on the proxy. Raise an
        exception if the interface cannot be resolved.
        """
        if len(self.interfaces) == 1:
            interface, signature = self.interfaces[0]
        else:
            def find_interface(interface):
                for iface,signature in self.interfaces:
                    if iface == interface:
                        return signature
            for interface in self.proxy.interfaces:
                signature = find_interface(interface)
                if signature:
                    break
            else:
                raise dbusx.Error('could not determine interface')
        return interface, signature

    def __call__(self, *args, **kwargs):
        """Call a D-BUS method. See :class:`MethodStub` for the calling
        convention. The *signature* and *interface* keyword arguments can be
        used to override the signature and the interface. Other keyword
        arguments are passed directly to :meth:`Connection.call_method`.
        """
        interface = kwargs.pop('interface', None)
        signature = kwargs.pop('signature', None)
        if interface is None and signature is None:
            interface, signature = self._resolve_interface()
        elif interface is None:
            interface, _ = self._resolve_interface()
        elif signature is None:
            _, signature = self._resolve_interface()
        reply = self.proxy.connection.call_method(self.proxy.service,
                        self.proxy.path, interface, self.method,
                        signature=signature, args=args, **kwargs)
        if not isinstance(reply, dbusx.MessageBase):
            return reply
        self.proxy.message = reply
        if reply.type == dbusx.MESSAGE_TYPE_ERROR:
            if reply.error_name == dbusx.ERROR_TIMEOUT:
                raise dbusx.TimeoutError(reply.error_name)
            else:
                raise dbusx.RemoteError(reply.error_name)
        args = reply.args
        if len(args) == 0:
            args = None
        elif len(args) == 1:
            args = args[0]
        return args


class SignalStub(object):
    """Signal handler stub.

    Instances of this class are added to proxy instances. Handlers can
    subsequently be added via the :meth:`connect` method.
    """

    def __init__(self, proxy, interface, signal):
        self.proxy = proxy
        self.interfaces = [interface]
        self.signal = signal

    def add_interface(self, interface):
        self.interfaces.append(interface)

    def _get_interface(self):
        if len(self.interfaces) == 1:
            interface = self.interfaces[0]
        else:
            for interface in self.proxy.interfaces:
                if interface in self.interfaces:
                    break
            else:
                raise dbusx.Error('could not determine interface')
        return interface

    def connect(self, callback, interface=None):
        """Connect a signal to a callback.

        If the signal is raised, *callback* will be called. The callback will
        be passed two arguments: a :class:`Connection` instance, and
        :class:`Message` instance. These correspond to the connection the
        signal was received on, and the signal message, respectively.

        Normally the *interface* keyword argument is not needed. It is only
        needed in case there are multiple interfaces that emit the same signal,
        and furthermore the interface search path on the proxy does not resolve
        which interface to use. In this case, you need to specify the
        interface. If you don't, an exception will be raised.
        """
        if interface is None:
            interface = self._get_interface()
        def call_handler(message):
            self.proxy.message = message
            callback(*message.args)
        self.proxy.connection.connect_to_signal(self.proxy.service,
                    self.proxy.path, interface, self.signal, call_handler)


class Proxy(object):
    """D-BUS proxy object.

    A DBusProxy is a stub to an object on the message bus. The object
    is identified by its bus name and its path.
    """

    def __init__(self, connection, service, path, interfaces=None,
                 introspect=True):
        self.connection = connection
        self.service = service
        self.path = path
        self.interfaces = interfaces or ()
        self.logger = dbusx.util.getLogger('dbusx.Proxy', context=self)
        if introspect:
            self._introspect()

    def __str__(self):
        s = 'dbusx.Proxy(service=%s, path=%s, interfaces=%s)' % \
                (repr(self.service), repr(self.path), repr(self.interfaces))
        return s

    def add_method(self, interface, name, signature):
        """Add new method to this proxy."""
        method = getattr(self, name, None)
        if method and isinstance(method, MethodStub):
            method.add_interface(interface, signature)
        elif method is None:
            method = MethodStub(self, interface, name, signature)
            setattr(self, name, method)

    def add_signal(self, interface, name):
        """Add a new signal to this proxy."""
        signal = getattr(self, name, None)
        if signal and isinstance(signal, SignalStub):
            signal.add_interface(interface)
        elif signal is None:
            signal = SignalStub(self, interface, name)
            setattr(self, name, signal)

    def _introspect(self):
        """Use the org.freedesktop.DBus.Introspectable interface to enumerate
        methods and signals supported for the remote object, and define them on
        the proxy.
        """
        log = self.logger
        reply = self.connection.call_method(self.service, self.path,
                        dbusx.INTERFACE_INTROSPECTABLE, 'Introspect')
        if reply.type == dbusx.MESSAGE_TYPE_ERROR:
            error = reply.error_name
            if error == dbusx.ERROR_UNKNOWN_INTERFACE:
                log.debug('Object %s:%s does not support introspection',
                          self.service, self.path)
            else:
                log.error('Error introspecting object %s:%s: %s',
                           self.service, self.path, error)
            return
        args = reply.args
        if len(args) != 1 or not isinstance(args[0], six.string_types):
            log.error('Illegal reply for "Introspect" method')
            return
        try:
            doc = etree.fromstring(args[0])
        except etree.ParseError as e:
            log.error('XML error in Introspect reply: %s', str(e))
            return
        nmethods = nsignals = 0
        for ifnode in doc.findall('./interface'):
            ifname = ifnode.attrib.get('name')
            if not ifname:
                continue
            for mnode in ifnode.findall('./method'):
                mname = mnode.attrib.get('name')
                if not mname:
                    continue
                # ElementTree in Python 2.6 does not support the
                # @attribute='value' predicate on a path so manually weed
                # out the direction="in" arguments
                args_in = ''.join([anode.attrib.get('type', '')
                                   for anode in mnode.findall('./arg')
                                   if anode.attrib.get('direction') == 'in'])
                self.add_method(ifname, mname, args_in)
                nmethods += 1
            for signode in ifnode.findall('./signal'):
                signame = signode.attrib.get('name')
                if not signame:
                    continue
                self.add_signal(ifname, signame)
                nsignals += 1
        log.debug('added %d methods and %d signals', nmethods, nsignals)

    def _get_message(self):
        return getattr(self.connection.local, 'message', None)

    def _set_message(self, value):
        self.connection.local.message = value

    message = property(_get_message, _set_message)
