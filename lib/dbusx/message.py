#
# This file is part of python-dbusx. Python-dbusx is free software
# available under the terms of the MIT license. See the file "LICENSE" that
# was provided together with this source file for the licensing terms.
#
# Copyright (c) 2012-2013 the python-dbusx authors. See the file "AUTHORS"
# for a complete list.

import dbusx


class Message(dbusx.MessageBase):
    """
    A Message that is sent or received on a D-BUS connection.
    A message consists of header fields and arguments. Both are exposed as
    attributes on instances of this class.
    """

    def __init__(self, type, no_reply=False, no_auto_start=False, serial=None,
                 reply_serial=None, path=None, interface=None, member=None,
                 error_name=None, destination=None, sender=None):
        """Create a new message.

        Only the *type* argument is mandatory, all other arguments are
        optional. The arguments have the same meaning as their respective class
        attributes.

        When using this constructor, it is your responsibility to check that
        the required arguments for the specific message type are set. If you
        fail to do this, sending out this message on a connection will fail.

        For more user-friendly constructors, see :meth:`method_call` and
        :meth:`signal`.
        """
        super(Message, self).__init__(type)
        self.no_reply = no_reply
        self.no_auto_start = no_auto_start
        if serial is not None:
            self.serial = serial
        if reply_serial is not None:
            self.reply_serial = reply_serial
        if path is not None:
            self.path = path
        if interface is not None:
            self.interface = interface
        if member is not None:
            self.member = member
        if error_name is not None:
            self.error_name = error_name
        if destination is not None:
            self.destination = destination
        if sender is not None:
            self.sender = sender

    @classmethod
    def method_call(cls, service, path, interface, method, signature=None,
                    args=None):
        """Create a new METHOD_CALL message.

        This creates a method call for the method *method* on interface
        *interface* at the bus name *service* under the path *path*.
        """
        message = cls(dbusx.MESSAGE_TYPE_METHOD_CALL, destination=service,
                      path=path, interface=interface, member=method)
        if signature is not None:
            message.set_args(signature, messge)
        return message

    @classmethod
    def signal(cls, service, path, interface, signal, signature=None,
               args=None):
        """Create a new SIGNAL message.

        This represents a signal with name *signal* on interface *interface*
        that is emitted by the service at bus name *service* under path *path*.
        """
        message = cls(dbusx.MESSAGE_TYPE_SIGNAL, destination=service,
                      path=path, interface=interface, member=signal)
        if signature is not None:
            message.set_args(signature, args)
        return message

    def reply(self, signature=None, args=None):
        """Create a METHOD_RETURN message.

        The message represents a reply to the current message, which must be a
        METHOD_CALL message.
        """
        if self.type != dbusx.MESSAGE_TYPE_METHOD_CALL:
            raise TypeError('Cannot create reply to message of type %s'
                            % self.type)
        message = type(self)(dbusx.MESSAGE_TYPE_METHOD_RETURN,
                             reply_serial=self.serial, destination=self.sender)
        if signature is not None:
            message.set_args(signature, args)
        return message

    def error_reply(self, error_name, signature=None, args=None):
        """Create an ERROR message.

        The message represents an error reply to the current message, which
        must be a METHOD_CALL message.
        """
        if self.type != dbusx.MESSAGE_TYPE_METHOD_CALL:
            raise TypeError('Cannot create error reply to message of type %s'
                            % self.type)
        message = type(self)(dbusx.MESSAGE_TYPE_ERROR, error_name=error_name,
                             reply_serial=self.serial, destination=self.sender)
        if signature is not None:
            message.set_args(signature, args)
        return message
