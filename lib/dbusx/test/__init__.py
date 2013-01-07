#
# This file is part of python-dbusx. Python-dbusx is free software
# available under the terms of the MIT license. See the file "LICENSE" that
# was provided together with this source file for the licensing terms.
#
# Copyright (c) 2012 the python-dbusx authors. See the file "AUTHORS" for a
# complete list.

from __future__ import print_function

import os
import re
import subprocess
import signal
import tempfile
import logging.config

import dbusx
from nose import SkipTest


def assert_raises(exc, func, *args, **kwargs):
    """Like nose.tools.assert_raises but returns the exception."""
    try:
        func(*args, **kwargs)
    except Exception as e:
        if isinstance(e, exc):
            return e
        raise
    raise AssertionError('%s not raised' % exc.__name__)


def check_output(command):
    """Python 2.6 does not have a subprocess.check_output()."""
    process = subprocess.Popen(command, stdout=subprocess.PIPE)
    output, err = process.communicate()
    status = process.poll()
    if status:
        raise RuntimeError('"%s" exited with status %s', (command, status))
    return output


class UnitTest(object):
    """Test infrastructure for dbusx tests."""

    # Set to false if test doesn't require a bus daemon
    need_dbus = True

    _re_assign = re.compile(r'^([a-zA-Z_][a-zA-Z0-9_]*)=' \
                            r'''([^"']*?|'[^']*'|"([^"\\]|\\.)*");?$''')

    @classmethod
    def setup_class(cls):
        cls._have_bus_daemon = False
        if cls.need_dbus:
            cls._start_bus_daemon()

    @classmethod
    def _start_bus_daemon(cls):
        # Launch a session bus for our tests
        abspath = os.path.abspath(__file__)
        cfgname = os.path.join(os.path.dirname(abspath), 'dbus.conf')
        try:
            output = check_output(['dbus-launch', '--config-file', cfgname,
                                   '--sh-syntax'])
        except OSError:
            raise SkipTest('dbus-launch is required for running this test')
        except subprocess.CalledProcessError as e:
            raise SkipTest('dbus-launch exited with error %s' % e.returncode)
        for line in output.splitlines():
            line = line.decode('utf-8')  # assume for now.. could parse $LANG
            mobj = cls._re_assign.match(line)
            if not mobj:
                continue
            key = mobj.group(1)
            value = mobj.group(2)
            if value.startswith('"') or value.startswith("'"):
                value = value[1:-1]
            if key == 'DBUS_SESSION_BUS_ADDRESS':
                dbusx.BUS_SESSION = value
                os.environ['DBUS_SESSION_BUS_ADDRESS'] = value
            elif key == 'DBUS_SESSION_BUS_PID':
                cls._bus_daemon_pid = int(value)
        cls._have_bus_daemon = True

    @classmethod
    def teardown_class(cls):
        if not cls._have_bus_daemon:
            return
        os.kill(cls._bus_daemon_pid, signal.SIGTERM)


#testdir = os.path.split(__file__)[0]
#logconf = os.path.join(testdir, 'logging.conf')
#logging.config.fileConfig(logconf)
