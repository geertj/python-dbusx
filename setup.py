#
# This file is part of python-dbusx. Python-dbusx is free software
# available under the terms of the MIT license. See the file "LICENSE" that
# was provided together with this source file for the licensing terms.
#
# Copyright (c) 2012-2013 the python-dbusx authors. See the file "AUTHORS" for
# a complete list.

import sys
import subprocess
from setuptools import setup, Extension


version_info = {
    'name': 'python-dbusx',
    'version': '0.9',
    'description': 'An alternate interface to D-BUS supporting evented IO',
    'author': 'Geert Jansen',
    'author_email': 'geertj@gmail.com',
    'url': 'https://github.com/geertj/python-dbusx',
    'license': 'MIT',
    'classifiers': [
        'Development Status :: 4 - Beta',
        'License :: OSI Approved :: MIT License',
        'Operating System :: POSIX',
        'Programming Language :: Python :: 2.6',
        'Programming Language :: Python :: 2.7',
        'Programming Language :: Python :: 3.2',
        'Programming Language :: Python :: 3.3'
    ]
}


def check_output(command):
    """Python 2.6 does not have a subprocess.check_output()."""
    process = subprocess.Popen(command, stdout=subprocess.PIPE)
    output, err = process.communicate()
    status = process.poll()
    if status:
        raise RuntimeError('"%s" exited with status %s', (command, status))
    return output


def pkgconfig(*args):
    """Run pkg-config."""
    command = ['pkg-config'] + list(args)
    try:
        output = check_output(command)
    except OSError:
        sys.stderr.write('error: command \'%s\' not found\n' % command[0])
        sys.exit(1)
    except subprocess.CalledProcessError as e:
        sys.stderr.write('error: command \'%s\' failed with status %s\n'
                         % (' '.join(command), e.returncode))
        sys.stderr.write('Make sure libdbus and its header files are installed,\n')
        sys.stderr.write('and that pkg-config is able to find it.\n')
        sys.exit(1)
    return str(output.decode('ascii')).strip().split()


if __name__ == '__main__':
    setup(
        package_dir = { '': 'lib' },
        packages = ['dbusx', 'dbusx.test'],
        ext_modules = [Extension('dbusx._dbus', ['lib/dbusx/_dbus.c'],
                  extra_compile_args = pkgconfig('--cflags', 'dbus-1'),
                  extra_link_args =  pkgconfig('--libs', 'dbus-1'))],
        install_requires = ['six', 'setuptools'],
        test_suite = 'nose.collector',
        **version_info
    )
