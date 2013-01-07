Overview
========

Python-dbusx is a alternative Python interface for D-BUS that supports evented
IO. It builds directly on top of libdbus and has no other dependencies.

Building and Installing
=======================

::

 $ python setup.py build
 $ sudo python setup.py install

Requirements
============

* Python 2.6, 2.7 or 3.3.
* To use evented IO, you need an event loop adater that supports the
  `EventLoop` interface from the upcoming PEP 3156. The `looping` package
  provides adapters for libev and libuv. See https://github.com/geertj/looping.

Comments and Suggestion
=======================

Feel free to add an issue on the Github site. Also feel free to send me an
email on geertj@gmail.com.
