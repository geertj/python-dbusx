#
# This file is part of python-dbusx. Python-dbusx is free software
# available under the terms of the MIT license. See the file "LICENSE" that
# was provided together with this source file for the licensing terms.
#
# Copyright (c) 2012-2013 the python-dbusx authors. See the file "AUTHORS"
# for a complete list.

import dbusx
import dbusx.util
import time
import threading


class Connection(dbusx.ConnectionBase):
    """A connection to the D-BUS.
    
    This class represents a connection to a desktop bus (D-BUS). It has two
    constructors. The default constructor `Connection()` will create a new
    private connection. An alternate constructor `Connection.get()` is
    available that create a connection that is shared with other users of
    libdbus (Python and non-Python) in the current process.

    To connect to a D-BUS instance, you need to specify its address to one of
    the constructors or to the :meth:`open` method. The most common D-BUS
    instances are the system and session bus. To connect to these buses, you
    can use the special constants dbusx.BUS_SYSTEM and dbusx.BUS_SESSION as the
    address, respectively. To connect to another bus, specify the address as a
    string.

    By default, this class will use the event loop provided by the package
    `looping.tulip`. This provides a good basic event loop based on the Python
    built-in select/poll/epoll/kqueue multiplexers. The event loop can be
    overrided by passing a different one to one of the constructors. It is your
    responsiblity to make sure the event loop is run.

    You can also use an event loop that is external to dbusx. In this case, you
    need to specify the parameter `install_event_loop=False` toathe constructor.
    """

    def __init__(self, address):
        """Create a new private connection.

        If *address* is provided, the connection will opened to the specified
        address.
        """
        super(Connection, self).__init__(address)
        self.context = None
        self._registered = False
        self._signal_handlers = []
        self.logger = dbusx.util.getLogger('dbusx.Connection',
                                           context=str(self))
        self.local = self._local()

    def __str__(self):
        s = 'Connection(address=%s, shared=%s)' % (self.address, self.shared)
        return s

    def proxy(self, service, path, interfaces=None):
        """Return a proxy for an object on the D-BUS.

        The *service* argument specifies the bus name of the remote object and
        *path* specifies its path. The *interfaces* argument, if provided,
        specifies the interface search path used when resolving methods. You
        only need to provide this if the object exposes methods or signals with
        the same name on multiple interfaces.
        """
        return dbusx.Proxy(self, service, path, interfaces)

    def publish(self, instance, path):
        """Publish a Python object instance on the D-BUS.

        The *instance* parameter can be a :class:`dbusx.Object` instance, or
        any other instance. In the latter case, the object is automatically
        wrapped to an Object instance using :meth:`dbusx.Object.wrap`. The
        *path* parameter specifies the path to publish this object at. It may
        contain a trailing '*' to indicate that the object will also handle all
        paths below the indicated path.
        """
        if not isinstance(instance, dbusx.Object):
            instance = dbusx.Object.wrap(instance)
        instance.register(self, path)
        fallback = path.endswith('*')
        path = path.rstrip('/*')
        self.register_object_path(path, instance._process, fallback)

    def remove(self, path):
        """Remove a published Python object.

        An object should have been previously published  at *path* using
        :meth:`publish`. An error will be raised if this is not the case.
        """
        path = path.rstrip('/*')
        self.unregister_object_path(path)

    def call_method(self, service, path, interface, method, signature=None,
                    args=None, no_reply=False, callback=None, timeout=None):
        """Call the method *method* on the interface *interface* of the remote
        object at bus name *service* at path *path*.

        If *signature* is specified, it must be a D-BUS signature string
        describing the input arguments of the method. In this case, *args* must
        be a tuple containing the argument values.

        If *callback* is not provided, this method performs a synchronous
        method call. It will block until a reply is received. The return value
        is a tuple containing the the return values of the remote method. In
        case of an error, a :class:`dbusx.Error` instance is raised.  The
        actual message of the response is available in the :attr:`reply`
        attribute. Subsequent calls will overwrite this attribute.

        If *callback* is provided, this method performs an asynchronous method
        call. The method call message will be queued, after which this method
        will return immediately. There is no return value.  At a later time,
        when the message is sent out and a reply is received, the callback will
        be called with two parameters: the connection and the message. In case
        of an error, this message will have the type
        `dbusx.MESSAGE_TYPE_ERROR`.  The internal :class:`PendingCall` instance
        that is used to track the response is available at the :attr:`pending`
        attribute. Subsequent calls will overwrite this attribute.

        The *no_reply* argument will set a flag in the D-BUS message indicating
        that no reply is required. In this case, a synchronous method call will
        still block, but only until the message has been sent out. An
        asynchronous method call will never block.

        The *timeout* parameter specifies the timeout in seconds to wait for a
        reply. The timeout may be an int or float. If no timeout is provided, a
        suitable default timeout is used. If no response is received within the
        timeout, a "org.freedesktop.DBus.Error.Timeout" error is generated.
        """
        message = dbusx.Message(dbusx.MESSAGE_TYPE_METHOD_CALL,
                        no_reply=no_reply, destination=service,
                        path=path, interface=interface, member=method)
        if signature is not None:
            message.set_args(signature, args)
        if callback is not None:
            # Fire a callback for the reply. Note that this requires event
            # loop integration otherwise the callback will never be called.
            self.send_with_reply(message, callback, timeout)
        elif no_reply:
            # No reply needed but block until flushed
            self.send(message)
            if not self.loop:
                self.flush()
        else:
            # Block for the reply
            replies = []
            def callback(message):
                replies.append(message)
            self.send_with_reply(message, callback, timeout)
            if timeout is not None:
                end_time = time.time() + timeout
            while not replies:
                secs = None if timeout is None else end_time - time.time()
                if self.loop:
                    if self.dispatch_status == dbusx.DISPATCH_DATA_REMAINS:
                        self.dispatch()
                    else:
                        self.loop.run_once(secs)
                else:
                    self.read_write_dispatch(secs)
            assert len(replies) == 1
            reply = replies[0]
            assert reply.type in (dbusx.MESSAGE_TYPE_METHOD_RETURN,
                                  dbusx.MESSAGE_TYPE_ERROR)
            assert reply.reply_serial == message.serial
            return reply

    def connect_to_signal(self, service, path, interface, signal, callback):
        """Install a signal handler for the signal *signal* that is raised on
        *interface* by the remote object at bus name *service* and path *path*.

        The *callback* argument must be a callable Python object. When a
        matching signal arrives, the callback is called with two arguments:
        the connection on which the signal was received, and the D-BUS message
        containing the signal.
        """
        if not self._registered:
            self.add_filter(self._signal_handler)
            self._registered = True
        self._signal_handlers.append((service, path, interface,
                                      signal, callback))
        # Call the "AddMatch" method on the D-BUS so that the signal specified
        # will get routed to us. Signals are normally sent out as multicast
        # messages and therefore an explicit route is required.
        # NOTE: It is OK to do this multiple times for the same signal.
        message = dbusx.Message(dbusx.MESSAGE_TYPE_METHOD_CALL, no_reply=True,
                          destination=dbusx.SERVICE_DBUS, path=dbusx.PATH_DBUS,
                          interface=dbusx.INTERFACE_DBUS, member='AddMatch')
        rule = "type='signal'"
        rule += ",sender='%s'" % service
        rule += ",path='%s'" % path
        rule += ",interface='%s'" % interface
        rule += ",member='%s'" % signal
        message.set_args('s', (rule,))
        self.send(message)

    def _signal_handler(self, connection, message):
        """Filter handler that is used to call signal handlers that are
        registered with connect_to_signal().
        """
        log = self.logger
        if connection is not self:
            log.error('_signal_handler: connection is not self??')
            return False
        if message.type != dbusx.MESSAGE_TYPE_SIGNAL:
            return False
        for service,path,interface,signal,callback in self._signal_handlers:
            if message.sender != service \
                    or message.path != path \
                    or message.interface != interface \
                    or message.member != signal:
                continue
            try:
                self._spawn(callback, message)
            except Exception as e:
                log.error('exception in signal handler', exc_info=True)
        # Allow others to see this signal as well
        return False

    def _spawn(self, function, *args):
        """Helper to spawn a function in a new context.

        By default this just executes function(*args), but it can be
        reimplemented by subclasses to add different spawning behavior.
        """
        return function(*args)

    def _local(self):
        """Helper to return a context-local storage object.

        This method must work in tandem with :meth:`_spawn()` so that the
        object implements local storage for the type of context implemented.
        """
        return threading.local()
