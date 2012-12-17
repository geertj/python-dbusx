/*
 * This file is part of python-dbusx. Python-dbusx is free software
 * available under the terms of the MIT license. See the file "LICENSE" that
 * was provided together with this source file for the licensing terms.
 *
 * Copyright (c) 2012 the python-dbusx authors. See the file "AUTHORS" for a
 * complete list.
 *
 * This file implements the "_dbus" module. It exposes parts of the libdbus
 * API to Python.
 */

#include <Python.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>

#include <dbus/dbus.h>


/*
 * We only support Python >= 2.6. Supporting older Python versions makes
 * supporting Python 3 too difficult.
 */

#if PY_MAJOR_VERSION == 2 && PY_MINOR_VERSION < 6
#  error "Python >= 2.6 is required"
#endif

/* Globals */

static PyObject *Error = NULL;
static int slot_self = -1;


/*
 * A few macros to make error handling in Python C extensions less verbose.
 */

#define RETURN_ERROR(fmt, ...) \
    do { \
        if ((fmt) != NULL) PyErr_Format(Error, fmt, ## __VA_ARGS__); \
        goto error; \
    } while (0)

#define RETURN_TYPE_ERROR(fmt, ...) \
    do { \
        PyErr_Format(PyExc_TypeError, fmt, ## __VA_ARGS__); \
        RETURN_ERROR(NULL); \
    } while (0)

#define RETURN_VALUE_ERROR(fmt, ...) \
    do { \
        PyErr_Format(PyExc_ValueError, fmt, ## __VA_ARGS__); \
        RETURN_ERROR(NULL); \
    } while (0)

#define RETURN_MEMORY_ERROR(err) \
    do { PyErr_NoMemory(); RETURN_ERROR(NULL); } while (0)

#define RETURN_ERROR_FROM_DBUS(err) \
    do { \
        if (dbus_error_is_set(&err)) RETURN_ERROR("dbus: %s", err.message); \
        else RETURN_ERROR("Unknown error"); \
    } while (0)

#define ASSERT(cond) \
    do { if (!(cond)) { \
        PyErr_SetString(PyExc_AssertionError, "Assertion failed " #cond); \
        RETURN_ERROR(NULL); \
    } } while (0)

#define INCREF(o) (Py_INCREF((PyObject *) o), o)
#define DECREF_SET_NULL(o) do { Py_DECREF(o); o = NULL; } while (0)

#define PRINT_AND_CLEAR_IF_ERROR(msg) \
    do { if (PyErr_Occurred()) { \
        PySys_WriteStderr("Uncaught exception in " msg ":\n"); \
        PyErr_PrintEx(1); PyErr_Clear(); \
    } } while (0)


/*
 * Python 2/3 compatibility macros.
 * Kudos to http://python3porting.com for many of the tips/tricks.
 */

#if PY_MAJOR_VERSION >= 3
#  define MOD_OK(val) (val)
#  define MOD_ERROR NULL
#  define MOD_INITFUNC(name) PyMODINIT_FUNC PyInit_ ## name(void)
#  define INIT_MODULE(mod, name, doc, methods) \
        do { \
            static struct PyModuleDef moduledef = { \
                PyModuleDef_HEAD_INIT, name, doc, -1, methods, }; \
            mod = PyModule_Create(&moduledef); \
        } while (0)
#else
#  define MOD_OK(value)
#  define MOD_ERROR
#  define MOD_INITFUNC(name) void init ## name(void)
#  define INIT_MODULE(mod, name, doc, methods) \
          do { mod = Py_InitModule3(name, methods, doc); } while (0)
/* Define a PyUnicode_Check that allows its argument to be a regular string
 * as well. Since it seems impossible to redefine a macro in terms of it's
 * previous definition, we need to undefine and then redefine it here.
 */
#  undef PyUnicode_Check
#  define _PyUnicode_Check(obj) PyType_FastSubclass(Py_TYPE(obj), Py_TPFLAGS_UNICODE_SUBCLASS)
#  define PyUnicode_Check(obj) (_PyUnicode_Check(obj) || PyString_Check(obj))
/* Same for PyLong_Check (also accept int). */
#  undef PyLong_Check
#  define _PyLong_Check(obj)  PyType_FastSubclass(Py_TYPE(obj), Py_TPFLAGS_LONG_SUBCLASS)
#  define PyLong_Check(obj) (_PyLong_Check(obj) || PyInt_Check(obj))
#endif


#if PY_MAJOR_VERSION < 3 || (PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION < 3)
/*
 * BIG FAT WARNING about using this function: the return string is inside
 * a static buffer!! Subsequent calls of this function will invalidate
 * the buffer. This is *UNLIKE* the Python 3.3 version which uses a buffer
 * allocated as part of the unicode object.
 */
static char *
PyUnicode_AsUTF8(PyObject *obj)
{
    char *ret;
    static PyObject *Pbytes = NULL;

#if PY_MAJOR_VERSION < 3
    if (PyString_Check(obj)) {
        char *ptr = PyString_AsString(obj);
        ret = ptr;
        while (*ptr++) {
            if (*ptr & 0x80)
                RETURN_ERROR("non-ascii characters in input");
        }
    } else
#endif
    if (PyUnicode_Check(obj)) {
        if (Pbytes != NULL)
            Py_DECREF(Pbytes);
        if ((Pbytes = PyUnicode_AsUTF8String(obj)) == NULL)
            RETURN_ERROR(NULL);
        ret = PyBytes_AsString(Pbytes);
    } else
        RETURN_ERROR("expecting string input");
    return ret;

error:
    return NULL;
}
#endif

#if PY_MAJOR_VERSION < 3
/* All PyLong_AsXXX() functions seem to accept an int as well on
 * Python 2.x, except PyLong_AsUnsignedLongLong.. */
#define PyLong_AsUnsignedLongLong(arg) \
    PyInt_Check(arg) ? PyLong_AsUnsignedLong(arg) : PyLong_AsUnsignedLongLong(arg)
#endif

/************************************************************************
 * Utility functions and objects.
 */

static void
decref(void *data)
{
    Py_DECREF((PyObject *) data);
}

/*
 * We define various, strict checks for paths, interfaces, members,
 * etc. It is necessary to check these here, because libdbus will call
 * abort() (!) when we pass it an illegal value (presumably for security
 * reasons). See the D-BUS specification for the requirements.
 */

static int
_check_bus_name(const char *name)
{
    int i, ndots=0;

    if (name[0] == '.')
        return 0;

    for (i=0; name[i] != '\000'; i++) {
        if (!(isalpha(name[i]) || name[i] == '_' || name[i] == '-' ||
              (name[i] == '.' && name[i-1] != '.' && name[i-1] != ':') ||
              (name[i] == ':' && i == 0) ||
              (isdigit(name[i]) && i > 0 && (name[i-1] != '.' || name[0] == ':'))))
            return 0;
        if (name[i] == '.') ndots++;
    }
    if (name[i-1] == '.' || ndots == 0 || i > DBUS_MAXIMUM_NAME_LENGTH)
        return 0;
    return 1;
}

static int
_check_path(const char *path)
{
    int i;

    if (path[0] != '/')
        return 0;
    for (i=1; path[i] != '\000'; i++) {
        if (!(isalnum(path[i]) || path[i] == '_' ||
              (path[i] == '/' && path[i-1] != '/')))
            return 0;
    }
    if (i > 1 && path[i-1] == '/')
        return 0;
    return 1;
}

static int
_check_interface(const char *interface)
{
    int i, ndots=0;

    if (!(isalpha(interface[0]) || interface[0] == '_'))
        return 0;
    for (i=1; interface[i] != '\000'; i++) {
        if (!(isalpha(interface[i]) || interface[i] == '_' ||
              (interface[i] == '.' && interface[i-1] != '.') ||
              (isdigit(interface[i]) && interface[i-1] != '.')))
            return 0;
        if (interface[i] == '.') ndots++;
    }
    if (interface[i-1] == '.' || ndots == 0 || i > DBUS_MAXIMUM_NAME_LENGTH)
        return 0;
    return 1;
}

static int
_check_member(const char *member)
{
    int i;
    
    if (!(isalpha(member[0]) || member[0] == '_'))
        return 0;
    for (i=1; member[i] != '\000'; i++) {
        if (!(isalnum(member[i]) || member[i] == '_'))
            return 0;
    }
    if (i > DBUS_MAXIMUM_NAME_LENGTH)
        return 0;
    return 1;
}


/*
 * This method parses a D-BUS signature string at `signature`, and returns a pointer
 * to the first character after the end of the first full type. For example, if
 * the signature string is "aaii", the pointer will be to the second "i".
 */

static char *
get_one_full_type(char *signature)
{
    int depth;
    char *end, endtype = '\000';

    switch (*signature) {
    case DBUS_TYPE_ARRAY:
        end = get_one_full_type(signature+1);
        break;
    case DBUS_STRUCT_BEGIN_CHAR:
        endtype = DBUS_STRUCT_END_CHAR;
        break;
    case DBUS_DICT_ENTRY_BEGIN_CHAR:
        endtype = DBUS_DICT_ENTRY_END_CHAR;
        break;
    case '\000':
        end = NULL;
        break;
    default:
        end = signature+1;
        break;
    }

    if (endtype != '\000') {
        depth = 1; end = signature;
        while (*++end != '\000' && depth > 0) {
            if (*end == *signature) depth++;
            else if (*end == endtype) depth--;
        }
        if (depth)
            RETURN_ERROR("unbalanced `%c' format", *signature);
    }
    return end;

error:
    return NULL;
}

static int
_check_signature(const char *signature, int arraydepth, int structdepth)
{
    char *start, *end, *ptr, store;

    /* We do modify `signature` temporarily during processing but we restore it
     * before we return. The source for signature is an internal buffer in
     * PyUnicode or PyBytes so that officially not allowed. However given
     * that we do not call out to any Python code before we return, it should
     * be fine. */

    ptr = (char *) signature;
    start = end = ptr;
    while (*ptr != '\000') {
        end = get_one_full_type(ptr);
        if (end == NULL) return 0;
        if (end - ptr == 1) {
            if (!strchr("ybnqiuxtdsogvh", *ptr)) return 0;
        } else if (*ptr == DBUS_TYPE_ARRAY) {
            if (arraydepth >= 32) return 0;
            store = *end; *end = '\000';
            if (!_check_signature(ptr+1, arraydepth+1, structdepth))
                return 0;
            *end = store;
        } else if (*ptr == DBUS_STRUCT_BEGIN_CHAR ||
                    *ptr == DBUS_DICT_ENTRY_BEGIN_CHAR) {
            if (structdepth >= 32) return 0;
            store = *--end; *end = '\000';
            if (!_check_signature(ptr+1, arraydepth, structdepth+1))
                return 0;
            *end++ = store;
            if ((*ptr == DBUS_STRUCT_BEGIN_CHAR &&
                        store != DBUS_STRUCT_END_CHAR) ||
                    (*ptr == DBUS_DICT_ENTRY_BEGIN_CHAR &&
                            store != DBUS_DICT_ENTRY_END_CHAR))
                return 0;
        }
        ptr = end;
    }
    if (end - start > 255)
        return 0;
    return 1;
}

/*
 * Valid numerical ranges for the D-BUS integer types.
 */

static PyObject **check_number_cache = NULL;
static char *check_numbers[11] = {
    "0x0", "0xff", "0xffff",
    "0xffffffff", "0xffffffffffffffff",
    "-0x8000", "0x7fff",
    "-0x80000000", "0x7fffffff",
    "-0x8000000000000000", "0x7fffffffffffffff"
};

static int
init_check_number_cache(void)
{
    int i;
    PyObject *Pnumber;

    if ((check_number_cache = calloc(11, sizeof(PyObject *))) == NULL)
        RETURN_MEMORY_ERROR();
    for (i=0; i<11; i++) {
        Pnumber = PyLong_FromString(check_numbers[i], NULL, 0);
        if (Pnumber == NULL)
            RETURN_ERROR(NULL);
        check_number_cache[i] = Pnumber;
    }
    return 1;

error:
    if (check_number_cache != NULL) free(check_number_cache);
    return 0;
}

/*
 * Check if a Python int or long object is in the valid range for a
 * D-BUS numeric types (byte, int16, etc).
 */

static int
check_number(PyObject *number, int type)
{
    PyObject *Pmin,  *Pmax;

    if (!PyLong_Check(number))
        RETURN_TYPE_ERROR("expecting integer argument for `%c' format", type);

    switch (type) {
    case DBUS_TYPE_BYTE:
        Pmin = check_number_cache[0];
        Pmax = check_number_cache[1];
        break;
    case DBUS_TYPE_UINT16:
        Pmin = check_number_cache[0];
        Pmax = check_number_cache[2];
        break;
    case DBUS_TYPE_UINT32:
        Pmin = check_number_cache[0];
        Pmax = check_number_cache[3];
        break;
    case DBUS_TYPE_UINT64:
        Pmin = check_number_cache[0];
        Pmax = check_number_cache[4];
        break;
    case DBUS_TYPE_INT16:
        Pmin = check_number_cache[5];
        Pmax = check_number_cache[6];
        break;
    case DBUS_TYPE_INT32:
        Pmin = check_number_cache[7];
        Pmax = check_number_cache[8];
        break;
    case DBUS_TYPE_INT64:
        Pmin = check_number_cache[9];
        Pmax = check_number_cache[10];
        break;
    default:
        return 0;
    }

    if (PyObject_RichCompareBool(number, Pmin, Py_LT) == 1 ||
                PyObject_RichCompareBool(number, Pmax, Py_GT) == 1)
        RETURN_VALUE_ERROR("value out of range for `%c' format", type);
    return 1;

error:
    return 0;
}


/**********************************************************************
 * Watch object: used for event loop integration
 */

typedef struct
{
    PyObject_HEAD
    DBusWatch *watch;
    PyObject *reader;
    PyObject *writer;
} WatchObject;

static PyTypeObject WatchType =
{
    PyVarObject_HEAD_INIT(NULL, 0)
    "Watch",
    sizeof(WatchObject)
};

PyDoc_STRVAR(watch_doc,
    "Wraps a DBusWatch object for event loop callbacks.\n");

static int
watch_traverse(WatchObject *self, visitproc visit, void *arg)
{
    if (self->reader != NULL)
        Py_VISIT(self->reader);
    if (self->writer != NULL)
        Py_VISIT(self->writer);
    return 0;
}

static int
watch_clear(WatchObject *self)
{
    if (self->reader != NULL) 
        DECREF_SET_NULL(self->reader);
    if (self->writer != NULL)
        DECREF_SET_NULL(self->writer);
    return 0;
}

static void
watch_dealloc(WatchObject *self)
{
    watch_clear(self);
    Py_TYPE(self)->tp_free(self);
}

PyDoc_STRVAR(watch_handle_doc,
    "handle(flags)\n\n"
    "Must be called by an event loop when the file descriptor has become\n"
    "ready. The *flags* argument must indicate DBUS_WATCH_READABLE and/or\n"
    "DBUS_WATCH_WRITABLE.\n");

static PyObject *
watch_handle(WatchObject *self, PyObject *args)
{
    int flags;

    if (!PyArg_ParseTuple(args, "i:handle", &flags))
        return NULL;

    if (dbus_watch_handle(self->watch, flags) == FALSE)
        RETURN_MEMORY_ERROR();

    Py_RETURN_NONE;

error:
    return NULL;
}

static PyMethodDef watch_methods[] =
{
    { "handle", (PyCFunction) watch_handle, METH_VARARGS, watch_handle_doc },
    { NULL }
};

static PyObject *
watch_type_init()
{
    WatchType.tp_doc = watch_doc;
    WatchType.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC;
    WatchType.tp_new = PyType_GenericNew;
    WatchType.tp_dealloc = (destructor) watch_dealloc;
    WatchType.tp_traverse = (traverseproc) watch_traverse;
    WatchType.tp_clear = (inquiry) watch_clear;
    WatchType.tp_methods = watch_methods;
    if (PyType_Ready(&WatchType) < 0)
        return NULL; 
    return (PyObject *) &WatchType;
}


/**********************************************************************
 * Timeout object. Used for event loop integration
 */

typedef struct
{
    PyObject_HEAD
    DBusTimeout *timeout;
    PyObject *dcall;
} TimeoutObject;

static PyTypeObject TimeoutType =
{
    PyVarObject_HEAD_INIT(NULL, 0)
    "Timeout",
    sizeof(TimeoutObject)
};

PyDoc_STRVAR(timeout_doc,
    "Wraps a DBusTimer object for event loop callbacks.\n");

static int
timeout_traverse(TimeoutObject *self, visitproc visit, void *arg)
{
    if (self->dcall)
        Py_VISIT(self->dcall);
    return 0;
}

static int
timeout_clear(TimeoutObject *self)
{
    if (self->dcall)
        DECREF_SET_NULL(self->dcall);
    return 0;
}

static void
timeout_dealloc(TimeoutObject *self)
{
    timeout_clear(self);
    Py_TYPE(self)->tp_free(self);
}

PyDoc_STRVAR(timeout_handle_doc,
    "handle()\n\n"
    "Must be called by the event loop when the timeout has expired.\n"
    "The timeout should automatically restart.\n");

static PyObject *
timeout_handle(TimeoutObject *self, PyObject *args)
{
    if (!PyArg_ParseTuple(args, ":handle"))
        return NULL;

    ASSERT(self->dcall != NULL);
    if (!dbus_timeout_handle(self->timeout))
        RETURN_MEMORY_ERROR();

    Py_RETURN_NONE;

error:
    return NULL;
}

static PyMethodDef timeout_methods[] = \
{
    { "handle", (PyCFunction) timeout_handle, METH_VARARGS,
            timeout_handle_doc },
    { NULL }
};

static PyObject *
timeout_type_init()
{
    TimeoutType.tp_doc = timeout_doc;
    TimeoutType.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC;
    TimeoutType.tp_new = PyType_GenericNew;
    TimeoutType.tp_dealloc = (destructor) timeout_dealloc;
    TimeoutType.tp_traverse = (traverseproc) timeout_traverse;
    TimeoutType.tp_clear = (inquiry) timeout_clear;
    TimeoutType.tp_methods = timeout_methods;
    if (PyType_Ready(&TimeoutType) < 0)
        return NULL; 
    return (PyObject *) &TimeoutType;
}


/**********************************************************************
 * Message object. This is one of the key objects (the other is Connection).
 * A message corresponds to a D-BUS message sent over a D-BUS connection.
 * It contains methods for setting and retrieving header values and arguments.
 */

typedef struct
{
    PyObject_HEAD
    DBusMessage *message;
} MessageObject;

PyTypeObject MessageType =
{
    PyVarObject_HEAD_INIT(NULL, 0)
    "MessageBase",
    sizeof(MessageObject)
};

PyDoc_STRVAR(message_doc,
    "This class wraps a DBusMessage structure from libdbus and\n"
    "various functions that operate on it. Its purpose is to serve\n"
    "as a base class for creating the Python-level Mesage classes.\n");

static int
message_init(MessageObject *self, PyObject *args, PyObject *kwargs)
{
    int type;
    static char *kwlist[] = { "type", NULL };

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "i", kwlist, &type))
        return -1;

    if (type <= DBUS_MESSAGE_TYPE_INVALID || type >= DBUS_NUM_MESSAGE_TYPES)
        RETURN_VALUE_ERROR("illegal message type: %d", type);
    if ((self->message = dbus_message_new(type)) == NULL)
        RETURN_MEMORY_ERROR();
    return 0;

error:
    return -1;
}

static void
message_dealloc(MessageObject *self)
{
    if (self->message) {
        dbus_message_unref(self->message);
        self->message = NULL;
    }
    Py_TYPE(self)->tp_free(self);
}


PyDoc_STRVAR(message_type_doc,
    "The message type. One of MESSAGE_TYPE_INVALID,\n"
    "MESSAGE_TYPE_METHOD_CALL, MESSAGE_TYPE_METHOD_RETURN,\n"
    "MESSAGE_TYPE_ERROR or MESSAGE_TYPE_SIGNAL. This attribute\n"
    "is read-only and can be set only in the constructor.\n");

static PyObject *
message_get_type(MessageObject *self, void *context)
{
    if (self->message == NULL)
        RETURN_ERROR("uninitialized message");
    return PyLong_FromLong(dbus_message_get_type(self->message));
error:
    return NULL;
}

PyDoc_STRVAR(message_no_reply_doc,
    "Boolean indicating no reply is needed for a method call.");

static PyObject *
message_get_no_reply(MessageObject *self, void *context)
{
    if (self->message == NULL)
        RETURN_ERROR("uninitialized message");
    return PyBool_FromLong(dbus_message_get_no_reply(self->message));
error:
    return NULL;
}

static int
message_set_no_reply(MessageObject *self, PyObject *value,
                            void *context)
{
    if (self->message == NULL)
        RETURN_ERROR("uninitialized message");
    if (!PyLong_Check(value))
        RETURN_TYPE_ERROR("expecting an integer");
    dbus_message_set_no_reply(self->message,
                              (dbus_bool_t) PyLong_AsLong(value));
    return 0;
error:
    return -1;
}

PyDoc_STRVAR(message_no_auto_start_doc,
    "Boolean requesting to not automatically start a service.");

static PyObject *
message_get_no_auto_start(MessageObject *self, void *context)
{
    if (self->message == NULL)
        RETURN_ERROR("uninitialized message");
    return PyBool_FromLong(!dbus_message_get_auto_start(self->message));
error:
    return NULL;
}

static int
message_set_no_auto_start(MessageObject *self, PyObject *value,
                          void *context)
{
    if (self->message == NULL)
        RETURN_ERROR("uninitialized message");
    if (!PyLong_Check(value))
        RETURN_TYPE_ERROR("expecting an integer");
    dbus_message_set_auto_start(self->message, !PyLong_AsLong(value));
    return 0;
error:
    return -1;
}

PyDoc_STRVAR(message_serial_doc,
    "The message serial number. For outgoing messages it will be\n"
    "automatically generated (if unset).\n");

static PyObject *
message_get_serial(MessageObject *self, void *context)
{
    unsigned long serial;
    if (self->message == NULL)
        RETURN_ERROR("uninitialized message");
    if ((serial = dbus_message_get_serial(self->message)) != 0)
        return PyLong_FromUnsignedLong(serial);
    Py_RETURN_NONE;
error:
    return NULL;
}

static int
message_set_serial(MessageObject *self, PyObject *value,
                   void *context)
{
    unsigned long serial;

    if (self->message == NULL)
        RETURN_ERROR("uninitialized message");
    if (!PyLong_Check(value))
        RETURN_TYPE_ERROR("expecting an integer");
    if (!check_number(value, 'u'))
        RETURN_ERROR(NULL);
    if ((serial = PyLong_AsUnsignedLong(value)) == 0)
        RETURN_VALUE_ERROR("serial must be > 0");
    dbus_message_set_serial(self->message, (dbus_uint32_t) serial);
    return 0;
error:
    return -1;
}

PyDoc_STRVAR(message_reply_serial_doc,
    "The serial of the message this message is a reply to.\n"
    "Requiredd for method returns and errors.\n");

static PyObject *
message_get_reply_serial(MessageObject *self, void *context)
{
    unsigned long reply_serial;
    if (self->message == NULL)
        RETURN_ERROR("uninitialized message");
    if ((reply_serial = dbus_message_get_reply_serial(self->message)) != 0)
        return PyLong_FromLong(reply_serial);
    Py_RETURN_NONE;

error:
    return NULL;
}

static int
message_set_reply_serial(MessageObject *self, PyObject *value,
                                void *context)
{
    unsigned long serial;

    if (self->message == NULL)
        RETURN_ERROR("uninitialized message");
    if (!PyLong_Check(value))
        RETURN_TYPE_ERROR("expecting an integer");
    if (!check_number(value, 'u'))
        RETURN_ERROR(NULL);
    if ((serial = PyLong_AsUnsignedLong(value)) == 0)
        RETURN_VALUE_ERROR("reply_serial must be > 0");
    dbus_message_set_reply_serial(self->message, (dbus_uint32_t) serial);
    return 0;
error:
    return -1;
}

PyDoc_STRVAR(message_path_doc,
    "The object path. Required for method calls and signals.\n");

static PyObject *
message_get_path(MessageObject *self, void *context)
{
    const char *path;
    if (self->message == NULL)
        RETURN_ERROR("uninitialized message");
    if ((path = dbus_message_get_path(self->message)) != NULL)
        return PyUnicode_FromString(path);
    Py_RETURN_NONE;
error:
    return NULL;
}

static int
message_set_path(MessageObject *self, PyObject *value,
                  void *context)
{
    const char *path;
    if (self->message == NULL)
        RETURN_ERROR("uninitialized message");
    if (!PyUnicode_Check(value))
        RETURN_TYPE_ERROR("'path': expecting a string");
    if ((path = PyUnicode_AsUTF8(value)) == NULL)
        RETURN_ERROR(NULL);
    if (!_check_path(path))
        RETURN_VALUE_ERROR("'path': illegal path");
    if (!dbus_message_set_path(self->message, path))
        RETURN_MEMORY_ERROR();
    return 0;
error:
    return -1;
}

PyDoc_STRVAR(message_interface_doc,
    "The interface. Required for signals.\n");

static PyObject *
message_get_interface(MessageObject *self, void *context)
{
    const char *interface;
    if (self->message == NULL)
        RETURN_ERROR("uninitialized message");
    if ((interface = dbus_message_get_interface(self->message)) != NULL)
        return PyUnicode_FromString(interface);
    Py_RETURN_NONE;
error:
    return NULL;
}

static int
message_set_interface(MessageObject *self, PyObject *value,
                             void *context)
{
    const char *interface;
    if (self->message == NULL)
        RETURN_ERROR("uninitialized message");
    if (!PyUnicode_Check(value))
        RETURN_TYPE_ERROR("'interface': expecting a string");
    if ((interface = PyUnicode_AsUTF8(value)) == NULL)
        RETURN_ERROR(NULL);
    if (!_check_interface(interface))
        RETURN_VALUE_ERROR("'interface': illegal interface");
    if (!dbus_message_set_interface(self->message, interface))
        RETURN_MEMORY_ERROR();
    return 0;
error:
    return -1;
}

PyDoc_STRVAR(message_member_doc,
    "The method or signal name. Requird for method calls and signals.\n");

static PyObject *
message_get_member(MessageObject *self, void *context)
{
    const char *member;
    if (self->message == NULL)
        RETURN_ERROR("uninitialized message");
    if ((member = dbus_message_get_member(self->message)) != NULL)
        return PyUnicode_FromString(member);
    Py_RETURN_NONE;
error:
    return NULL;
}

static int
message_set_member(MessageObject *self, PyObject *value,
                   void *context)
{
    const char *member;
    if (self->message == NULL)
        RETURN_ERROR("uninitialized message");
    if (!PyUnicode_Check(value))
        RETURN_TYPE_ERROR("'member': expecting a string");
    if ((member = PyUnicode_AsUTF8(value)) == NULL)
        RETURN_ERROR(NULL);
    if (!_check_member(member))
        RETURN_VALUE_ERROR("'interface': illegal interface");
    if (!dbus_message_set_member(self->message, member))
        RETURN_MEMORY_ERROR();
    return 0;
error:
    return -1;
}

PyDoc_STRVAR(message_error_name_doc,
    "The error name. Required for error messages.\n");

static PyObject *
message_get_error_name(MessageObject *self, void *context)
{
    const char *error_name;
    if (self->message == NULL)
        RETURN_ERROR("uninitialized message");
    if ((error_name = dbus_message_get_error_name(self->message)) != NULL)
        return PyUnicode_FromString(error_name);
    Py_RETURN_NONE;
error:
    return NULL;
}

static int
message_set_error_name(MessageObject *self, PyObject *value,
                              void *context)
{
    const char *error_name;
    if (self->message == NULL)
        RETURN_ERROR("uninitialized message");
    if (!PyUnicode_Check(value))
        RETURN_TYPE_ERROR("'error_name': expecting a string");
    if ((error_name = PyUnicode_AsUTF8(value)) == NULL)
        RETURN_ERROR(NULL);
    if (!_check_interface(error_name))
        RETURN_VALUE_ERROR("'error_name': illegal error name");
    if (!dbus_message_set_error_name(self->message, error_name))
        RETURN_MEMORY_ERROR();
    return 0;
error:
    return -1;
}

PyDoc_STRVAR(message_sender_doc,
    "Unique name of the sending connection. This is set automatically by\n"
    "the message bus and so it is reliable (no spoofing possible).\n");

static PyObject *
message_get_sender(MessageObject *self, void *context)
{
    const char *sender;
    if (self->message == NULL)
        RETURN_ERROR("uninitialized message");
    if ((sender = dbus_message_get_sender(self->message)) != NULL)
        return PyUnicode_FromString(sender);
    Py_RETURN_NONE;
error:
    return NULL;
}

static int
message_set_sender(MessageObject *self, PyObject *value,
                          void *context)
{
    const char *sender;
    if (self->message == NULL)
        RETURN_ERROR("uninitialized message");
    if (!PyUnicode_Check(value))
        RETURN_TYPE_ERROR("'sender': expecting a string");
    if ((sender = PyUnicode_AsUTF8(value)) == NULL)
        RETURN_ERROR(NULL);
    if (!_check_bus_name(sender))
        RETURN_VALUE_ERROR("illegal sender: %s", sender);
    if (!dbus_message_set_sender(self->message, sender))
        RETURN_MEMORY_ERROR();
    return 0;
error:
    return -1;
}

PyDoc_STRVAR(message_destination_doc,
    "The destination to send this message to.\n");

static PyObject *
message_get_destination(MessageObject *self, void *context)
{
    const char *destination;
    if (self->message == NULL)
        RETURN_ERROR("uninitialized message");
    if ((destination = dbus_message_get_destination(self->message)) != NULL)
        return PyUnicode_FromString(destination);
    Py_RETURN_NONE;
error:
    return NULL;
}

static int
message_set_destination(MessageObject *self, PyObject *value,
                               void *context)
{
    const char *destination;
    if (self->message == NULL)
        RETURN_ERROR("uninitialized message");
    if (!PyUnicode_Check(value))
        RETURN_TYPE_ERROR("'destination': expecting a string");
    if ((destination = PyUnicode_AsUTF8(value)) == NULL)
        RETURN_ERROR(NULL);
    if (!_check_bus_name(destination))
        RETURN_VALUE_ERROR("illegal destination: %s", destination);
    if (!dbus_message_set_destination(self->message, destination))
        RETURN_MEMORY_ERROR();
    return 0;
error:
    return -1;
}

PyDoc_STRVAR(message_signature_doc,
    "The signature of the arguments accompanying this message.\n"
    "This is a read-only attribute.\n");

static PyObject *
message_get_signature(MessageObject *self, void *context)
{
    const char *signature;
    if (self->message == NULL)
        RETURN_ERROR("uninitialized message");
    if ((signature = dbus_message_get_signature(self->message)) != NULL)
        return PyUnicode_FromString(signature);
    Py_RETURN_NONE;
error:
    return NULL;
}

/* DBusBasicValue is not public so redefine it here.  */

typedef union 
{
    dbus_bool_t bl;
    unsigned char u8;
    dbus_int16_t i16;
    dbus_uint16_t u16;
    dbus_int32_t i32;
    dbus_uint32_t u32;
    dbus_int64_t i64;
    dbus_uint64_t u64;
    char *str;
    double dbl;
} basic_value;


/* Forward declaration. */
static PyObject * message_read_args(DBusMessageIter *, int);

/*
 * This meaty function reads a single complete type from a D-BUS message
 * iterator, and returns the corresponding Python type. A single complete type
 * may be a basic type like 'i', but also a complex type like "array of array
 * of int" (aai). This function may recurse into itself for complex types.
 */

static PyObject *
message_read_arg(DBusMessageIter *iter, int depth)
{
    int type, subtype, size;
    char *sig = NULL, *ptr;
    PyObject *Parg = NULL, *Pitem = NULL, *Pkey = NULL, *Pvalue = NULL;
    basic_value value;
    DBusMessageIter subiter;

    type = dbus_message_iter_get_arg_type(iter);
    switch (type) {
    case DBUS_TYPE_BYTE:
        dbus_message_iter_get_basic(iter, &value);
        if ((Parg = PyLong_FromLong(value.u8)) == NULL)
            RETURN_ERROR(NULL);
        break;
    case DBUS_TYPE_BOOLEAN:
        dbus_message_iter_get_basic(iter, &value);
        if ((Parg = PyBool_FromLong(value.bl)) == NULL)
            RETURN_ERROR(NULL);
        break;
    case DBUS_TYPE_INT16:
        dbus_message_iter_get_basic(iter, &value);
        if ((Parg = PyLong_FromLong(value.i16)) == NULL)
            RETURN_ERROR(NULL);
        break;
    case DBUS_TYPE_UINT16:
        dbus_message_iter_get_basic(iter, &value);
        if ((Parg = PyLong_FromLong(value.u16)) == NULL)
            RETURN_ERROR(NULL);
        break;
    case DBUS_TYPE_INT32:
        dbus_message_iter_get_basic(iter, &value);
        if ((Parg = PyLong_FromLong(value.i32)) == NULL)
            RETURN_ERROR(NULL);
        break;
    case DBUS_TYPE_UINT32:
        dbus_message_iter_get_basic(iter, &value);
        if ((Parg = PyLong_FromUnsignedLong(value.u32)) == NULL)
            RETURN_ERROR(NULL);
        break;
    case DBUS_TYPE_INT64:
        dbus_message_iter_get_basic(iter, &value);
        if ((Parg = PyLong_FromLongLong(value.i64)) == NULL)
            RETURN_ERROR(NULL);
        break;
    case DBUS_TYPE_UINT64:
        dbus_message_iter_get_basic(iter, &value);
        if ((Parg = PyLong_FromUnsignedLongLong(value.u64)) == NULL)
            RETURN_ERROR(NULL);
        break;
    case DBUS_TYPE_DOUBLE:
        dbus_message_iter_get_basic(iter, &value);
        if ((Parg = PyFloat_FromDouble(value.dbl)) == NULL)
            RETURN_ERROR(NULL);
        break;
    case DBUS_TYPE_STRING:
        dbus_message_iter_get_basic(iter, &value);
        if ((Parg = PyUnicode_FromString(value.str)) == NULL)
            RETURN_ERROR(NULL);
        break;
    case DBUS_TYPE_OBJECT_PATH:
    case DBUS_TYPE_SIGNATURE:
        dbus_message_iter_get_basic(iter, &value);
        if ((Parg = PyUnicode_FromString(value.str)) == NULL)
            RETURN_ERROR(NULL);
        break;
    case DBUS_TYPE_STRUCT:
        dbus_message_iter_recurse(iter, &subiter);
        if ((Parg = message_read_args(&subiter, depth+1)) == NULL)
            RETURN_ERROR(NULL);
        break;
    case DBUS_TYPE_ARRAY:
        subtype = dbus_message_iter_get_element_type(iter);
        dbus_message_iter_recurse(iter, &subiter);
        if (subtype == DBUS_TYPE_BYTE) {
            dbus_message_iter_get_fixed_array(&subiter, &ptr, &size);
            if ((Parg = PyBytes_FromStringAndSize(ptr, size)) == NULL)
                RETURN_ERROR(NULL);
        } else {
            if (subtype == DBUS_TYPE_DICT_ENTRY)
                Parg = PyDict_New();
            else
                Parg = PyList_New(0);
            if (Parg == NULL)
                RETURN_ERROR(NULL);
            while (dbus_message_iter_get_arg_type(&subiter) != DBUS_TYPE_INVALID) {
                if ((Pitem = message_read_arg(&subiter, depth+1)) == NULL)
                    RETURN_ERROR(NULL);
                if (PyDict_Check(Parg)) {
                    ASSERT(PyTuple_Check(Pitem));
                    ASSERT(PyTuple_Size(Pitem) == 2);
                    if (PyDict_SetItem(Parg, PyTuple_GET_ITEM(Pitem, 0),
                                       PyTuple_GET_ITEM(Pitem, 1)) < 0)
                        RETURN_ERROR(NULL);
                } else
                    PyList_Append(Parg, Pitem);
                Py_DECREF(Pitem); Pitem = NULL;
                dbus_message_iter_next(&subiter);
            }
        }
        break;
    case DBUS_TYPE_DICT_ENTRY:
        dbus_message_iter_recurse(iter, &subiter);
        if ((Pkey = message_read_arg(&subiter, depth+1)) == NULL)
            RETURN_ERROR(NULL);
        if (!dbus_message_iter_next(&subiter))
            RETURN_ERROR("illegal dict_entry");
        if ((Pvalue = message_read_arg(&subiter, depth+1)) == NULL)
            RETURN_ERROR(NULL);
        if ((Parg = PyTuple_New(2)) == NULL)
            RETURN_ERROR(NULL);
        PyTuple_SET_ITEM(Parg, 0, Pkey);
        PyTuple_SET_ITEM(Parg, 1, Pvalue);
        break;
    case DBUS_TYPE_VARIANT:
        dbus_message_iter_recurse(iter, &subiter);
        if ((sig = dbus_message_iter_get_signature(&subiter)) == NULL)
            RETURN_MEMORY_ERROR();
        if ((Pkey = PyUnicode_FromString(sig)) == NULL)
            RETURN_ERROR(NULL);
        if ((Pvalue = message_read_arg(&subiter, depth+1)) == NULL)
            RETURN_ERROR(NULL);
        if ((Parg = PyTuple_New(2)) == NULL)
            RETURN_ERROR(NULL);
        PyTuple_SET_ITEM(Parg, 0, Pkey);
        PyTuple_SET_ITEM(Parg, 1, Pvalue);
        dbus_free(sig); sig = NULL;
        break;
    }

    return Parg;

error:
    if (Parg != NULL) Py_DECREF(Parg);
    if (Pitem != NULL) Py_DECREF(Pitem);
    if (Pkey != NULL) Py_DECREF(Pkey);
    if (Pvalue != NULL) Py_DECREF(Pvalue);
    if (sig != NULL) dbus_free(sig);
    return NULL;
}

static PyObject *
message_read_args(DBusMessageIter *iter, int depth)
{
    PyObject *Plist = NULL, *Pargs = NULL, *Parg = NULL;

    Plist = PyList_New(0);
    while (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_INVALID) {
        if ((Parg = message_read_arg(iter, depth)) == NULL)
            RETURN_ERROR(NULL);
        if (PyList_Append(Plist, Parg) < 0)
            RETURN_ERROR(NULL);
        Py_DECREF(Parg); Parg = NULL;
        dbus_message_iter_next(iter);
    }

    if ((Pargs = PyList_AsTuple(Plist)) == NULL)
        RETURN_ERROR(NULL);
    Py_DECREF(Plist);
    return Pargs;

error:
    if (Parg != NULL) Py_DECREF(Parg);
    if (Plist != NULL) Py_DECREF(Plist);
    if (Pargs != NULL) Py_DECREF(Pargs);
    return NULL;
}

PyDoc_STRVAR(message_args_doc,
    "The arguments accompanying this message.\n"
    "This is a read-only attribute.\n");

static PyObject *
message_get_args(MessageObject *self, void *context)
{
    PyObject *Pargs;
    DBusMessageIter iter;
    
    if (self->message == NULL)
        RETURN_ERROR("uninitialized object");
    if (dbus_message_iter_init(self->message, &iter))
        Pargs = message_read_args(&iter, 0);
    else
        Pargs = PyTuple_New(0);
    if (Pargs == NULL)
        RETURN_ERROR(NULL);
    return Pargs;

error:
    return NULL;
}


PyGetSetDef message_properties[] = \
{
    { "type", (getter) message_get_type, NULL, message_type_doc },
    { "no_reply", (getter) message_get_no_reply,
            (setter) message_set_no_reply, message_no_reply_doc },
    { "no_auto_start", (getter) message_get_no_auto_start,
            (setter) message_set_no_auto_start, message_no_auto_start_doc },
    { "serial", (getter) message_get_serial,
            (setter) message_set_serial, message_serial_doc },
    { "reply_serial", (getter) message_get_reply_serial,
            (setter) message_set_reply_serial, message_reply_serial_doc },
    { "path", (getter) message_get_path, (setter) message_set_path,
            message_path_doc },
    { "interface", (getter) message_get_interface,
            (setter) message_set_interface, message_interface_doc },
    { "member", (getter) message_get_member, (setter) message_set_member,
            message_member_doc },
    { "error_name", (getter) message_get_error_name,
            (setter) message_set_error_name, message_error_name_doc },
    { "sender", (getter) message_get_sender,
            (setter) message_set_sender, message_sender_doc },
    { "destination", (getter) message_get_destination,
            (setter) message_set_destination, message_destination_doc },
    { "signature", (getter) message_get_signature, NULL,
            message_signature_doc },
    { "args", (getter) message_get_args, NULL, message_args_doc },
    { NULL }
};


static int
message_append_args(DBusMessageIter *, char *, PyObject *, int);

/*
 * Another meaty function, this one to append a single complete argument to a
 * D-BUS message. Like with message_read_arg, this may recurse into
 * itself.
 */

static int
message_append_arg(DBusMessageIter *iter, char *signature,
                          PyObject *arg, int depth)
{
    int i, size; long l;
    char *subtype = NULL, *end, *ptr;
    PyObject *Parray, *Pitem = NULL, *Ptype = NULL, *Pvalue = NULL;
    basic_value value;
    DBusMessageIter subiter;

    switch (*signature) {
    case DBUS_TYPE_BYTE:
        if (!check_number(arg, *signature))
            RETURN_ERROR(NULL);
        value.u8 = PyLong_AsLong(arg);
        if (!dbus_message_iter_append_basic(iter, *signature, &value))
            RETURN_MEMORY_ERROR();
        break;
    case DBUS_TYPE_BOOLEAN:
        if ((l = PyObject_IsTrue(arg)) == -1)
            RETURN_ERROR(NULL);
        value.bl = (dbus_bool_t) l;
        if (!dbus_message_iter_append_basic(iter, *signature, &value))
            RETURN_MEMORY_ERROR();
        break;
    case DBUS_TYPE_INT16:
        if (!check_number(arg, *signature))
            RETURN_ERROR(NULL);
        value.i16 = PyLong_AsLong(arg);
        if (!dbus_message_iter_append_basic(iter, *signature, &value))
            RETURN_MEMORY_ERROR();
        break;
    case DBUS_TYPE_UINT16:
        if (!check_number(arg, *signature))
            RETURN_ERROR(NULL);
        value.u16 = PyLong_AsLong(arg);
        if (!dbus_message_iter_append_basic(iter, *signature, &value))
            RETURN_MEMORY_ERROR();
        break;
    case DBUS_TYPE_INT32:
        if (!check_number(arg, *signature))
            RETURN_ERROR(NULL);
        value.i32 = (dbus_int32_t) PyLong_AsLong(arg);
        if (!dbus_message_iter_append_basic(iter, *signature, &value))
            RETURN_MEMORY_ERROR();
        break;
    case DBUS_TYPE_UINT32:
        if (!check_number(arg, *signature))
            RETURN_ERROR(NULL);
        value.u32 = (dbus_uint32_t) PyLong_AsUnsignedLongMask(arg);
        if (!dbus_message_iter_append_basic(iter, *signature, &value))
            RETURN_MEMORY_ERROR();
        break;
    case DBUS_TYPE_INT64:
        if (!check_number(arg, *signature))
            RETURN_ERROR(NULL);
        value.i64 = (dbus_int64_t) PyLong_AsLongLong(arg);
        if (!dbus_message_iter_append_basic(iter, *signature, &value))
            RETURN_MEMORY_ERROR();
        break;
    case DBUS_TYPE_UINT64:
        if (!check_number(arg, *signature))
            RETURN_ERROR(NULL);
        value.u64 = (dbus_uint64_t) (PyLong_AsUnsignedLongLong(arg));
        if (!dbus_message_iter_append_basic(iter, *signature, &value))
            RETURN_MEMORY_ERROR();
        break;
    case DBUS_TYPE_DOUBLE:
        value.dbl = PyFloat_AsDouble(arg);
        if (PyErr_Occurred())
            RETURN_ERROR(NULL);
        if (!dbus_message_iter_append_basic(iter, *signature, &value))
            RETURN_MEMORY_ERROR();
        break;
    case DBUS_TYPE_OBJECT_PATH:
        if (!PyUnicode_Check(arg))
            RETURN_TYPE_ERROR("expecting str for `%c' format", *signature);
        if ((value.str = PyUnicode_AsUTF8(arg)) == NULL)
            RETURN_ERROR(NULL);
        if (!_check_path(value.str))
            RETURN_VALUE_ERROR("invalid object path argument");
        if (!dbus_message_iter_append_basic(iter, *signature, &value))
            RETURN_MEMORY_ERROR();
        break;
    case DBUS_TYPE_SIGNATURE:
        if (!PyUnicode_Check(arg))
            RETURN_TYPE_ERROR("expecting str for `%c' format", *signature);
        if ((value.str = PyUnicode_AsUTF8(arg)) == NULL)
            RETURN_ERROR(NULL);
        if (!_check_signature(value.str, 0, 0))
            RETURN_VALUE_ERROR("invalid signature");
        if (!dbus_message_iter_append_basic(iter, *signature, &value))
            RETURN_MEMORY_ERROR();
        break;
    case DBUS_TYPE_STRING:
        if (!PyUnicode_Check(arg))
            RETURN_TYPE_ERROR("expecting str for `%c' format", *signature);
        if ((value.str = PyUnicode_AsUTF8(arg)) == NULL)
            RETURN_ERROR(NULL);
        if (!dbus_message_iter_append_basic(iter, *signature, &value))
            RETURN_MEMORY_ERROR();
        break;
    case DBUS_STRUCT_BEGIN_CHAR:
        if (!dbus_message_iter_open_container(iter, DBUS_TYPE_STRUCT,
                    NULL, &subiter))
            RETURN_MEMORY_ERROR();
        if (!PySequence_Check(arg))
            RETURN_TYPE_ERROR("expecting sequence argument for struct format");
        if (!message_append_args(&subiter, signature+1, arg, depth+1))
            RETURN_ERROR(NULL);
        if (!dbus_message_iter_close_container(iter, &subiter))
            RETURN_MEMORY_ERROR();
        break;
    case DBUS_TYPE_ARRAY:
        if (!dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
                    signature+1, &subiter))
            RETURN_MEMORY_ERROR();
        if (signature[1] == DBUS_TYPE_BYTE) {
            if (!PyBytes_Check(arg))
                RETURN_TYPE_ERROR("expecting bytes argument for array of byte");
            ptr = PyBytes_AS_STRING(arg); size = (int) PyBytes_GET_SIZE(arg);
            if (!dbus_message_iter_append_fixed_array(&subiter, signature[1], &ptr, size))
                RETURN_MEMORY_ERROR();
        } else {
            if (signature[1] == DBUS_DICT_ENTRY_BEGIN_CHAR) {
                if (!PyDict_Check(arg))
                    RETURN_TYPE_ERROR("expecting dict argument for dict format");
                Parray = PyDict_Items(arg);
            } else {
                if (!PySequence_Check(arg))
                    RETURN_TYPE_ERROR("expecting sequence argument for array format");
                Parray = arg;
            }
            for (i=0; i<PySequence_Size(Parray); i++) {
                Pitem = PySequence_GetItem(Parray, i);
                if (!message_append_arg(&subiter, signature+1, Pitem, depth+1))
                    RETURN_ERROR(NULL);
                Py_DECREF(Pitem); Pitem = NULL;
            }
            if (Parray != arg)
                Py_DECREF(Parray);
        }
        if (!dbus_message_iter_close_container(iter, &subiter))
            RETURN_MEMORY_ERROR();
        break;
    case DBUS_DICT_ENTRY_BEGIN_CHAR:
        if (!dbus_message_iter_open_container(iter, DBUS_TYPE_DICT_ENTRY,
                    NULL, &subiter))
            RETURN_MEMORY_ERROR();
        if (!PySequence_Check(arg))
            RETURN_TYPE_ERROR("expecting sequence argument for dict_entry format");
        if (!message_append_args(&subiter, signature+1, arg, depth+1))
            RETURN_ERROR(NULL);
        if (!dbus_message_iter_close_container(iter, &subiter))
            RETURN_MEMORY_ERROR();
        break;
    case DBUS_TYPE_VARIANT:
        if (!PySequence_Check(arg))
            RETURN_TYPE_ERROR("expecting a sequence for variant");
        if (PySequence_Size(arg) != 2)
            RETURN_VALUE_ERROR("expecting a sequence of length 2 for variant");
        Ptype = PySequence_GetItem(arg, 0);
        Pvalue = PySequence_GetItem(arg, 1);
        if (!PyUnicode_Check(Ptype))
            RETURN_TYPE_ERROR("first item in sequence for variant must be string");
        if ((ptr = PyUnicode_AsUTF8(Ptype)) == NULL)
            RETURN_ERROR(NULL);
        /* On Python < 3.2, we provide our own version of PyUnicode_AsUTF8.
         * That version uses a single static PyObject to hold the return
         * value. As we recurse into ourselves here, this will not work and
         * we therefore need to copy the UTF8 buffer. */
        if ((subtype = strdup(ptr)) == NULL)
            RETURN_MEMORY_ERROR();
        if (!_check_signature(subtype, 0, 0))
            RETURN_VALUE_ERROR("invalid signature for variant");
        end = get_one_full_type(subtype);
        if (end == NULL || *end != '\000')
            RETURN_VALUE_ERROR("variant signature must be exactly one full type");
        if (!dbus_message_iter_open_container(iter, *signature, subtype, &subiter))
            RETURN_MEMORY_ERROR();
        if (!message_append_arg(&subiter, subtype, Pvalue, depth+1))
            RETURN_ERROR(NULL);
        if (!dbus_message_iter_close_container(iter, &subiter))
            RETURN_MEMORY_ERROR();
        Py_DECREF(Ptype); Ptype = NULL;
        Py_DECREF(Pvalue); Pvalue = NULL;
        free(subtype); subtype = NULL;
        break;
    default:
        RETURN_ERROR("unknown format character `%c'", *signature);
    }
    return 1;

error:
    if (Pitem != NULL) Py_DECREF(Pitem);
    if (Ptype != NULL) Py_DECREF(Ptype);
    if (Pvalue != NULL) Py_DECREF(Pvalue);
    if (subtype != NULL) free(subtype);
    return 0;
}

static int
message_append_args(DBusMessageIter *iter, char *signature,
                           PyObject *args, int depth)
{
    int curarg = 0;
    char *end, store;
    PyObject *Parg = NULL;

    while (*signature != '\000') {
        if ((end = get_one_full_type(signature)) == NULL)
            RETURN_ERROR(NULL);
        if (curarg == PySequence_Size(args))
            RETURN_TYPE_ERROR("too few arguments for signature string");
        store = *end; *end = '\000';
        Parg = PySequence_GetItem(args, curarg++);
        if (!message_append_arg(iter, signature, Parg, depth))
            RETURN_ERROR(NULL);
        Py_DECREF(Parg); Parg = NULL;
        *(signature = end) = store;
        if (*signature == DBUS_STRUCT_END_CHAR || *signature == DBUS_DICT_ENTRY_END_CHAR)
            signature++;
    }
    if (curarg != PySequence_Size(args))
        RETURN_TYPE_ERROR("too many arguments for signature string");
    return 1;

error:
    if (Parg != NULL) Py_DECREF(Parg);
    return 0;
}


PyDoc_STRVAR(message_set_args_doc,
    "set_args(signature, args)\n"
    "\n"
    "Set the message arguments to *args*, which must be a tuple containing\n"
    "the arguments. The arguments are converted to D-BUS types using the\n"
    "signature string provided in *signature*.\n");

static PyObject *
message_set_args(MessageObject *self, PyObject *args)
{
    char *signature, *ptr = NULL;
    DBusMessageIter iter;
    PyObject *Pargs;

    if (self->message == NULL)
        RETURN_ERROR("uninitialized object");
    if (!PyArg_ParseTuple(args, "sO:set_args", &signature, &Pargs))
        return NULL;
    if (!PySequence_Check(Pargs))
        RETURN_TYPE_ERROR("expecting a sequence for the arguments");
    if ((ptr = strdup(signature)) == NULL)
        RETURN_MEMORY_ERROR();
    if (!_check_signature(ptr, 0, 0))
        RETURN_VALUE_ERROR("illegal signature");

    dbus_message_iter_init_append(self->message, &iter);
    if (!message_append_args(&iter, ptr, Pargs, 0))
        RETURN_ERROR(NULL);

    free(ptr);
    Py_RETURN_NONE;

error:
    if (ptr != NULL) free(ptr);
    return NULL;
}

PyMethodDef message_methods[] = \
{
    { "set_args", (PyCFunction ) message_set_args, METH_VARARGS,
            message_set_args_doc },
    { NULL }
};

static PyObject *
message_type_init()
{
    MessageType.tp_doc = message_doc;
    MessageType.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE;
    MessageType.tp_new = PyType_GenericNew;
    MessageType.tp_init = (initproc) message_init;
    MessageType.tp_dealloc = (destructor) message_dealloc;
    MessageType.tp_methods = message_methods;
    MessageType.tp_getset = message_properties;
    if (PyType_Ready(&MessageType) < 0)
        return NULL; 
    return (PyObject *) &MessageType;
}


/**********************************************************************
 * Connection object. It wraps a DBusConnection structure, and
 * corresponds to a single (possibly shared) connection to the D-BUS.
 */

typedef struct
{
    PyObject_HEAD
    DBusConnection *connection;
    int shared;
    int skip_connect;
    PyObject *address;
    PyObject *loop;
    PyObject *filters;
    PyObject *object_paths;
} ConnectionObject;

PyTypeObject ConnectionType =
{
    PyVarObject_HEAD_INIT(NULL, 0)
    "ConnectionBase",
    sizeof(ConnectionObject)
};


/* Forward declarations */
static DBusConnection *_open_connection(PyObject *bus, int shared);
static int _close_connection(ConnectionObject *conn);


PyDoc_STRVAR(connection_doc,
        "Base functionality for creating Connenection classes.\n\n"
        "This class wraps a DBusConnection structure from libdbus and\n"
        "various functions that operate on it. Its purpose is to serve\n"
        "as a base class for creating Python-level Connection classes.\n");

static PyObject *
connection_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    ConnectionObject *Pconnection;

    if ((Pconnection = (ConnectionObject *)
                PyType_GenericNew(type, args, kwargs)) == NULL)
        RETURN_ERROR(NULL);
    if ((Pconnection->filters = PySet_New(NULL)) == NULL)
        RETURN_ERROR(NULL);
    if ((Pconnection->object_paths = PyDict_New()) == NULL)
        RETURN_ERROR(NULL);
    return (PyObject *) Pconnection;

error:
    return NULL;
}

static int
connection_init(ConnectionObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *bus;
    static char *kwlist[] = { "address", NULL };
    DBusConnection *connection;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O", kwlist, &bus))
        RETURN_ERROR(NULL);

    /* See note in connection_get() */
    if (self->skip_connect)
        return 0;

    if ((connection = _open_connection(bus, 0)) == NULL)
        RETURN_ERROR(NULL);
    if (!dbus_connection_set_data(connection, slot_self, self, decref))
        RETURN_ERROR("dbus_connection_set_data() failed");

    /* Account for the reference that DBusConnection has to to us. This
     * reference will prevent the connection from being garbage collected,
     * until it is taken back by our connection_close() method.
     *
     * Morale: use close() on your connections if you don't need them anymore.
     * If you don't, then even if a Connection become unaccessible, it will
     * not be garbage collected. */
    Py_INCREF(self);

    self->connection = connection;
    self->shared = 0;
    self->address = INCREF(bus);

    return 0;

error:
    return -1;
}

static void
connection_dealloc(ConnectionObject *self)
{
    _close_connection(self);
    DECREF_SET_NULL(self->filters);
    DECREF_SET_NULL(self->object_paths);

    Py_TYPE(self)->tp_free(self);
}

static int
connection_traverse(ConnectionObject *self, visitproc visit, void *arg)
{
    if (self->loop != NULL)
        Py_VISIT(self->loop);
    if (self->filters != NULL)
        Py_VISIT(self->filters);
    if (self->object_paths != NULL)
        Py_VISIT(self->object_paths);
    return 0;
}

static int
connection_clear(ConnectionObject *self)
{
    _close_connection(self);
    return 0;
}


PyDoc_STRVAR(connection_address_doc,
    "The connection address.");

static PyObject *
connection_get_address(ConnectionObject *self, void *context)
{
    if (self->address == NULL)
        Py_RETURN_NONE;

    return INCREF(self->address);
}


PyDoc_STRVAR(connection_shared_doc,
        "Is this a shared connection?");

static PyObject *
connection_get_shared(ConnectionObject *self, void *context)
{
    return PyBool_FromLong(self->shared);
}


PyDoc_STRVAR(connection_loop_doc,
    "The currently installed event loop, if any.\n");

static PyObject *
connection_get_loop(ConnectionObject *self, void *context)
{
    if (self->loop == NULL)
        Py_RETURN_NONE;

    return INCREF(self->loop);
}


PyDoc_STRVAR(connection_dispatch_status_doc,
    "The current dispatch status. This can be one of\n"
    "DBUS_DISPATCH_DATA_REMAINS, DBUS_DISPATCH_COMPLETE, or\n"
    "DBUS_DISPATCH_NEED_MEMORY.\n");

static PyObject *
connection_get_dispatch_status(ConnectionObject *self, void *context)
{
    int status;

    if (self->connection == NULL)
        Py_RETURN_NONE;

    status = dbus_connection_get_dispatch_status(self->connection);
    return PyLong_FromLong(status);
}


PyDoc_STRVAR(connection_unique_name_doc,
    "The unique name for this connection. Unique names\n"
    "start with a colon (\":\") and are automaticallly allocated\n"
    "by the message bus.\n");

static PyObject *
connection_get_unique_name(ConnectionObject *self, PyObject *args)
{
    const char *name;

    if (self->connection == NULL)
        Py_RETURN_NONE;

    if ((name = dbus_bus_get_unique_name(self->connection)) == NULL)
        RETURN_ERROR("dbus_bus_get_unique_name() failed");
    return PyUnicode_FromString(name);

error:
    return NULL;
}


static PyGetSetDef connection_properties[] = \
{
    { "address", (getter) connection_get_address, NULL,
                connection_address_doc },
    { "shared", (getter) connection_get_shared, NULL, connection_shared_doc },
    { "loop", (getter) connection_get_loop, NULL, connection_loop_doc },
    { "dispatch_status", (getter) connection_get_dispatch_status, NULL,
                connection_dispatch_status_doc },
    { "unique_name", (getter) connection_get_unique_name, NULL,
                connection_unique_name_doc },
    { NULL }
};

static DBusHandlerResult
handler_callback(DBusConnection *connection, DBusMessage *message,
                 void *data)
{
    int ret;
    PyObject *Presult, *Pconnection;
    MessageObject *Pmessage;

    Pconnection = (PyObject *) \
            dbus_connection_get_data(connection, slot_self);
    if (Pconnection == NULL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    Pmessage = (MessageObject *) \
            MessageType.tp_new(&MessageType, NULL, NULL);
    if (Pmessage == NULL)
        return DBUS_HANDLER_RESULT_NEED_MEMORY;
    Pmessage->message = dbus_message_ref(message);

    Presult = PyObject_CallFunction((PyObject *) data, "OO", Pconnection,
                                    Pmessage);
    Py_DECREF(Pmessage);
    if (Presult == NULL) {
        PyErr_Clear();
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    ret = PyObject_IsTrue(Presult) ? DBUS_HANDLER_RESULT_HANDLED 
                : DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    Py_DECREF(Presult);
    return ret;
}

static DBusConnection *
_open_connection(PyObject *bus, int shared)
{
    int id;
    char *address;
    DBusConnection *connection;
    DBusError error = DBUS_ERROR_INIT;

    /* Allow threads here because dbus_bus_register() will block until the
     * connection is established, authenticated, and registered. In case
     * of dbus_bus_get(), it calls dbus_bus_register() under the cover.
     *
     * The consequence is that if you're using an event loop, you need to
     * create your Connection instaces before entering the loop.
     */

    if (PyLong_Check(bus)) {
        id = (int) PyLong_AsLong(bus);
        Py_BEGIN_ALLOW_THREADS
        if (shared)
            connection = dbus_bus_get(id, &error);
        else
            connection = dbus_bus_get_private(id, &error);
        Py_END_ALLOW_THREADS
    } else if (PyUnicode_Check(bus)) {
        address = PyUnicode_AsUTF8(bus);
        Py_BEGIN_ALLOW_THREADS
        if (shared)
            connection = dbus_connection_open(address, &error);
        else
            connection = dbus_connection_open_private(address, &error);
        if (connection != NULL && !dbus_bus_register(connection, &error)) {
            if (!shared)
                dbus_connection_close(connection);
            dbus_connection_unref(connection);
            connection = NULL;
        }
        Py_END_ALLOW_THREADS
    } else
        RETURN_ERROR("Expecting a bus address or well known bus id");

    if (connection == NULL)
        RETURN_ERROR_FROM_DBUS(error);

    /* Should this be unconditionally set to FALSE? */
    dbus_connection_set_exit_on_disconnect(connection, FALSE);

    /* hand over our connection reference to our caller. */
    return connection;

error:
    return NULL;
}

static int
_close_connection(ConnectionObject *conn)
{
    PyObject *Piter = NULL, *Pitem = NULL;

    ASSERT(conn->filters != NULL);
    ASSERT(conn->object_paths != NULL);
    if (conn->connection == NULL) {
        ASSERT(conn->loop == NULL);
        ASSERT(PySet_Size(conn->filters) == 0);
        ASSERT(PyDict_Size(conn->object_paths) == 0);
        return 0;
    }
    ASSERT(conn->address != NULL);

    /* Remove any callback that was installed by us (event loop, filters,
     * and object path handlers). */

    if (conn->loop != NULL) {
        dbus_connection_set_watch_functions(conn->connection,
                        NULL, NULL, NULL, NULL, NULL);
        dbus_connection_set_timeout_functions(conn->connection,
                        NULL, NULL, NULL, NULL, NULL);
        DECREF_SET_NULL(conn->loop);
    }

    if ((Piter = PyObject_GetIter(conn->filters)) == NULL)
        RETURN_ERROR(NULL);
    while ((Pitem = PyIter_Next(Piter)) != NULL) {
        dbus_connection_remove_filter(conn->connection, handler_callback,
                                      Pitem);
        DECREF_SET_NULL(Pitem);
    }
    PySet_Clear(conn->filters);

    if ((Piter = PyObject_GetIter(conn->object_paths)) == NULL)
        RETURN_ERROR(NULL);
    while ((Pitem = PyIter_Next(Piter)) != NULL) {
        if (!dbus_connection_unregister_object_path(conn->connection,
                            PyUnicode_AsUTF8(Pitem)))
            RETURN_ERROR("dbus_connection_unregister_object_path() failed");
        DECREF_SET_NULL(Pitem);
    }
    PyDict_Clear(conn->object_paths);

    /* Now we can close the connection. Do not close the underlying connection
     * if it is shared though. */

    /* The next line will drop the D-BUS reference to our connection object.
     * After this, the connection may be garbage collected if it becomes
     * unaccessible. */
    dbus_connection_set_data(conn->connection, slot_self, NULL, NULL);
    if (!conn->shared)
        dbus_connection_close(conn->connection);
    dbus_connection_unref(conn->connection);
    conn->connection = NULL;
    DECREF_SET_NULL(conn->address);
    conn->shared = 0;
    return 0;

error:
    Py_XDECREF(Piter);
    Py_XDECREF(Pitem);
    return -1;
}


PyDoc_STRVAR(connection_get_doc,
    "get(address, shared=True)\n\n"
    "Return a D-BUS connection that is connected to *address*. The address\n"
    "may be one of BUS_SYSTEM, BUS_SESSION or BUS_STARTER\n"
    "to connect to one of the well known bus instances, or a string with\n"
    "a D-BUS connection address. The *shared* argument, if provided,\n"
    "specifies if this may be a shared connection or not.\n");

static PyObject *
connection_get(PyTypeObject *cls, PyObject *args, PyObject *kwargs)
{
    int shared = 1;
    PyObject *bus, *Pargs = NULL;
    ConnectionObject *self;
    DBusConnection *connection;
    static char *kwlist[] = { "bus", "shared", NULL };

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|i:get", kwlist,
                                     &bus, &shared))
        RETURN_ERROR(NULL);

    if ((connection = _open_connection(bus, shared)) == NULL)
        RETURN_ERROR(NULL);

    self = dbus_connection_get_data(connection, slot_self);
    if (self == NULL) {
         /* Create new python object, by calling tp_new and tp_init. */
        if ((Pargs = PyTuple_New(0)) == NULL)
            RETURN_ERROR(NULL);
        if ((self = (ConnectionObject *) cls->tp_new(cls, Pargs, NULL)) == NULL)
            RETURN_ERROR(NULL);
        DECREF_SET_NULL(Pargs);
        /* We want to call the constructor here to allow derived classes to
         * do initialization, but we don't want our base constructor to connect
         * as we are already connected. The small hack below prevents that. */
        self->connection = connection;  /* hand over D-BUS reference */
        self->shared = shared;
        self->address = INCREF(bus);
        self->skip_connect = 1;
        if ((Pargs = PyTuple_New(1)) == NULL)
            RETURN_ERROR(NULL);
        PyTuple_SET_ITEM(Pargs, 0, INCREF(bus));
        if (cls->tp_init((PyObject *) self, Pargs, NULL) < 0)
            RETURN_ERROR(NULL);
        DECREF_SET_NULL(Pargs);
        /* Hand off the tp_new() reference to the D-BUS connection.
         * Also see note in connection_init() */
        if (!dbus_connection_set_data(connection, slot_self, self, decref))
            RETURN_ERROR("dbus_connection_set_data() failed");
    }

    /* Need a new reference because even if the connection was just created,
     * the tp_new() reference is now owned by the D-BUS connection. */
    Py_INCREF(self);
    return (PyObject *) self;

error:
    Py_XDECREF(Pargs);
    return NULL;
}


PyDoc_STRVAR(connection_close_doc,
    "close()\n\n"
    "Close a connection.");

static PyObject *
connection_close(ConnectionObject *self, PyObject *args)
{
    if (!PyArg_ParseTuple(args, ":close"))
        return NULL;
    if (_close_connection(self) < 0)
        return NULL;
    Py_RETURN_NONE;
}


PyDoc_STRVAR(connection_send_doc,
    "send(message)\n\n"
    "Send a message on this connection. The *message* parameter must be a\n"
    ":class:`dbusx.Message` instance. This method only queues the message\n"
    "and does not perform any actual IO. The message will be sent out at a\n"
    "later time either by the event loop. If no event loop is installed,\n"
    "the message may be sent out manually by calling :meth:`flush` or\n"
    ":meth:`read_write_dispatch`.\n");

static PyObject *
connection_send(ConnectionObject *self, PyObject *args)
{
    MessageObject *message;

    if (!PyArg_ParseTuple(args, "O!:send", &MessageType, &message))
        return NULL;
    if (self->connection == NULL)
        RETURN_ERROR("not connected");

    if (!dbus_connection_send(self->connection, message->message, NULL))
        RETURN_ERROR("dbus_connection_send() failed");

    Py_RETURN_NONE;

error:
    return NULL;
}


PyDoc_STRVAR(connection_send_with_reply_doc,
    "send_with_reply(message, callback, timeout=None)\n\n"
    "Send a message on this connnection.\n\n"
    "The *message* parameter must be a :class:`dbusx.Message` instance.\n"
    "The *callback* parameter is a callback that will be called when a\n"
    "reply is received. It will be called with a :class:`dbusx.Message`\n"
    "instance as its only argument containing the reply message. The\n"
    "*timeout* parameter specifies a timeout in seconds to wait for a reply.\n"
    "If no reply a received within the timeout, the callback will be called\n"
    "with a locally generated error reply message. The timeout may be an int\n"
    "or float. If no timeout is provided a sensible default value will be\n"
    "used.\n\n");

static void
pending_call_notify_callback(DBusPendingCall *pending, void *data)
{
    MessageObject *Pmessage;

    Pmessage = (MessageObject *) MessageType.tp_new(&MessageType, NULL, NULL);
    if (Pmessage == NULL)
        return;
    Pmessage->message = dbus_pending_call_steal_reply(pending);
    if (Pmessage->message == NULL)
        return;
    PyObject_CallFunction((PyObject *) data, "O", Pmessage);
    if (PyErr_Occurred())
        PyErr_Clear();
    Py_DECREF(Pmessage);
    dbus_pending_call_unref(pending);
}

static PyObject *
connection_send_with_reply(ConnectionObject *self, PyObject *args)
{
    int msecs, type;
    PyObject *timeout = NULL, *callback = NULL;
    MessageObject *message;
    DBusPendingCall *pending = NULL;

    if (!PyArg_ParseTuple(args, "O!O|O:send_with_reply", &MessageType,
                          &message, &callback, &timeout))
        return NULL;
    if (self->connection == NULL)
        RETURN_ERROR("not connected");

    type = dbus_message_get_type(message->message);
    if ((type != DBUS_MESSAGE_TYPE_METHOD_CALL) && (callback != NULL))
        RETURN_ERROR("expecting a METHOD_CALL message");

    if (!PyCallable_Check(callback))
        RETURN_ERROR("expecting a callable for 'callback'");

    if (timeout == NULL || timeout == Py_None)
        msecs = -1;
    else if (PyLong_Check(timeout))
        msecs = (int) (1000 * PyLong_AsLong(timeout));
    else if (PyFloat_Check(timeout))
        msecs = (int) (1000.0 * PyFloat_AsDouble(timeout));
    else
        RETURN_ERROR("expecing int, float or None for 'timeout'");
    if (msecs < 0) msecs = -1;

    if (!dbus_connection_send_with_reply(self->connection,
                message->message, &pending, msecs) || (pending == NULL))
        RETURN_ERROR("dbus_connection_send_with_reply() failed");
    if (!dbus_pending_call_set_notify(pending, pending_call_notify_callback,
                                      callback, decref))
        RETURN_MEMORY_ERROR();
    Py_INCREF(callback);

    Py_RETURN_NONE;

error:
    if (pending != NULL) dbus_pending_call_unref(pending);
    return NULL;
}


PyDoc_STRVAR(connection_flush_doc,
    "flush()\n\n"
    "Flush any messages that were queued but not yet sent out.\n"
    "This method will block until all output has been sent out.\n");

static PyObject *
connection_flush(ConnectionObject *self, PyObject *args)
{
    if (!PyArg_ParseTuple(args, ":flush"))
        return NULL;
    if (self->connection == NULL)
        RETURN_ERROR("not connected");
    dbus_connection_flush(self->connection);
    Py_INCREF(Py_None);
    return Py_None;

error:
    return NULL;
}


PyDoc_STRVAR(connection_dispatch_doc,
    "dispatch()\n\n"
    "Dispatch one incoming message, if available. Messages are dispatched\n"
    "in three steps. First, method call responses are dispatched to\n"
    "callbacks that were registered iwth send(). Second, messages of any\n"
    "type are passed to the installed message filters, in the order the\n"
    "filters were added. Finally, for method calls, if there is a registered\n"
    "object path handler that matches the message's path, the message is\n"
    "dispatched there. The first callback that accepts a message completes\n"
    "the dispatch.\n\n"
    "This method returns the new dispatch status of the connection.\n");

static PyObject *
connection_dispatch(ConnectionObject *self, PyObject *args)
{
    int status;

    if (!PyArg_ParseTuple(args, ":dispatch"))
        return NULL;
    if (self->connection == NULL) {
        /* Don't raise an exception here. This function may be called
         * from a callback that may be alive after the connection
         * has been closed. */
        Py_INCREF(Py_None);
        return Py_None;
    }

    status = dbus_connection_dispatch(self->connection);
    return PyLong_FromLong(status);
}


PyDoc_STRVAR(connection_read_write_dispatch_doc,
    "read_write_dispatch(timeout)\n\n"
    "Run one iteration of the built-in, blocking event loop, and then\n"
    "dispatch one message.\n\n"
    "The *timeout* argument specifies the timeout for the built-in event\n"
    "loop. Note that if this built-in loop blocks, it cannot be pre-empted,\n"
    "not even by e.g sending a signal to the current process. Therefore,\n"
    "the built-in loop should only be used in simple applications that do\n"
    "not need to multiplex multple things.\n\n");

static PyObject *
connection_read_write_dispatch(ConnectionObject *self, PyObject *args)
{
    int status, msecs;
    PyObject *timeout = NULL;

    if (!PyArg_ParseTuple(args, "|O:read_write_dispatch", &timeout))
        return NULL;
    if (self->connection == NULL)
        RETURN_ERROR("not connected");

    if (timeout == NULL || timeout == Py_None)
        msecs = 0;
    else if (PyLong_Check(timeout))
        msecs = (int) (1000 * PyLong_AsLong(timeout));
    else if (PyFloat_Check(timeout))
        msecs = (int) (1000.0 * PyFloat_AsDouble(timeout));
    else
        RETURN_ERROR("expecing int, float or None for 'timeout'");
    if (msecs < 0) msecs = -1;

    status = dbus_connection_read_write_dispatch(self->connection, msecs);
    return PyBool_FromLong(status);

error:
    return NULL;
}


PyDoc_STRVAR(connection_set_loop_doc,
    "set_loop(loop)\n\n"
    "Enable event loop integration for this connection. The *loop*\n"
    "parameter must be an :class:`looping.EventLoop` instance.\n");

static dbus_bool_t
add_watch_callback(DBusWatch *watch, void *data)
{
    int fd, flags, enabled;
    WatchObject *Pwatch = NULL;
    PyObject *Pcallback = NULL, *Pdcall, *loop = (PyObject *) data;

    ASSERT(dbus_watch_get_data(watch) == NULL);

    Pwatch = (WatchObject *) WatchType.tp_new(&WatchType, NULL, NULL);
    if (Pwatch == NULL)
        RETURN_ERROR(NULL);
    Pwatch->watch = watch;
    dbus_watch_set_data(watch, Pwatch, decref);  /* hand off ref to dbus */

    fd = dbus_watch_get_unix_fd(watch);
    if (fd == -1)
        fd = dbus_watch_get_socket(watch);
    flags = dbus_watch_get_flags(watch);
    enabled = dbus_watch_get_enabled(watch);

    Pcallback = PyObject_GetAttrString((PyObject *) Pwatch, "handle");
    if (Pcallback == NULL)
        RETURN_ERROR(NULL);

    if (enabled && (flags & DBUS_WATCH_READABLE)) {
        Pdcall = PyObject_CallMethod(loop, "add_reader", "iOi", fd, Pcallback,
                                     DBUS_WATCH_READABLE);
        if (Pdcall == NULL)
            RETURN_ERROR(NULL);
        Pwatch->reader = Pdcall;  /* hand off reference */
    }
    if ((flags & DBUS_WATCH_WRITABLE) && enabled) {
        Pdcall = PyObject_CallMethod(loop, "add_writer", "iOi", fd, Pcallback,
                                     DBUS_WATCH_WRITABLE);
        if (Pdcall == NULL)
            RETURN_ERROR(NULL);
        Pwatch->writer = Pdcall;  /* hand off reference */
    }

    Py_DECREF(Pcallback);
    return TRUE;

error:
    Py_XDECREF(Pcallback);
    PRINT_AND_CLEAR_IF_ERROR("add_watch_callback()");
    return FALSE;
}

static void
remove_watch_callback(DBusWatch *watch, void *data)
{
    int fd;
    WatchObject *Pwatch;
    PyObject *Pret, *loop = (PyObject *) data;

    Pwatch = dbus_watch_get_data(watch);
    ASSERT(Pwatch != NULL);

    fd = dbus_watch_get_unix_fd(watch);
    if (fd == -1)
        fd = dbus_watch_get_socket(watch);

    if (Pwatch->reader != NULL) {
        Pret = PyObject_CallMethod(loop, "remove_reader", "i", fd);
        Py_XDECREF(Pret);  /* print error on Pret == NULL below */
        DECREF_SET_NULL(Pwatch->reader);
    }
    if (Pwatch->writer != NULL) {
        Pret = PyObject_CallMethod(loop, "remove_writer", "i", fd);
        Py_XDECREF(Pret);  /* print error on Pret == NULL below */
        DECREF_SET_NULL(Pwatch->writer);
    }

    dbus_watch_set_data(watch, NULL, NULL);  /* drops ref to Pwatch */
    /* fall through */

error:
    PRINT_AND_CLEAR_IF_ERROR("remove_watch_callback()");
}

static void
watch_toggled_callback(DBusWatch *watch, void *data)
{
    int fd, flags, enabled;
    WatchObject *Pwatch;
    PyObject *Pcallback = NULL, *Pdcall, *Pret, *loop = (PyObject *) data;

    Pwatch = dbus_watch_get_data(watch);
    ASSERT(Pwatch != NULL);

    fd = dbus_watch_get_unix_fd(watch);
    if (fd == -1)
        fd = dbus_watch_get_socket(watch);
    flags = dbus_watch_get_flags(watch);
    enabled = dbus_watch_get_enabled(watch);

    Pcallback = PyObject_GetAttrString((PyObject *) Pwatch, "handle");
    if (Pcallback == NULL)
        RETURN_ERROR(NULL);

    if (enabled && (flags & DBUS_WATCH_READABLE)) {
        if (Pwatch->reader == NULL) {
            Pdcall = PyObject_CallMethod(loop, "add_reader", "iOi", fd,
                                         Pcallback, DBUS_WATCH_READABLE);
            if (Pdcall == NULL)
                RETURN_ERROR(NULL);
            Pwatch->reader = Pdcall;  /* hand over reference */
        }
    } else  {
        if (Pwatch->reader != NULL) {
            Pret = PyObject_CallMethod(loop, "remove_reader", "i", fd);
            if (Pret == NULL)
                RETURN_ERROR(NULL);
            DECREF_SET_NULL(Pwatch->reader);
        }
    }
    if (enabled && (flags & DBUS_WATCH_WRITABLE)) {
        if (Pwatch->writer == NULL) {
            Pdcall = PyObject_CallMethod(loop, "add_writer", "iOi", fd,
                                         Pcallback, DBUS_WATCH_WRITABLE);
            if (Pdcall == NULL)
                RETURN_ERROR(NULL);
            Pwatch->writer = Pdcall;  /* hand off reference */
        }
    } else  {
        if (Pwatch->writer != NULL) {
            Pret = PyObject_CallMethod(loop, "remove_writer", "i", fd);
            if (Pret == NULL)
                RETURN_ERROR(NULL);
            DECREF_SET_NULL(Pwatch->writer);
        }
    }

    Py_DECREF(Pcallback);
    return;

error:
    Py_XDECREF(Pcallback);
    PRINT_AND_CLEAR_IF_ERROR("watch_toggled_callback()");
}

static dbus_bool_t
add_timeout_callback(DBusTimeout *timeout, void *data)
{
    int enabled;
    float interval;
    TimeoutObject *Ptimeout;
    PyObject *Pcallback = NULL, *Pdcall, *loop = (PyObject *) data;

    ASSERT(dbus_timeout_get_data(timeout) == NULL);

    Ptimeout = (TimeoutObject *) TimeoutType.tp_new(&TimeoutType, NULL, NULL);
    if (Ptimeout == NULL)
        RETURN_ERROR(NULL);
    Ptimeout->timeout = timeout;
    /* Hand off Ptimeout reference to dbus. */
    dbus_timeout_set_data(timeout, Ptimeout, decref);

    enabled = dbus_timeout_get_enabled(timeout);
    if (enabled) {
        Pcallback = PyObject_GetAttrString((PyObject *) Ptimeout, "handle");
        if (Pcallback == NULL)
            RETURN_ERROR(NULL);
        interval = (float) dbus_timeout_get_interval(timeout) / 1000.0;
        Pdcall = PyObject_CallMethod(loop, "call_later", "ffO", interval,
                                     interval, Pcallback);
        if (Pdcall == NULL)
            RETURN_ERROR(NULL);
        Ptimeout->dcall = Pdcall;
        DECREF_SET_NULL(Pcallback);
    }

    return TRUE;

error:
    Py_XDECREF(Pcallback);
    PRINT_AND_CLEAR_IF_ERROR("add_timeout_callback()");
    return FALSE;
}

static void
remove_timeout_callback(DBusTimeout *timeout, void *data)
{
    TimeoutObject *Ptimeout;
    PyObject *Pret = NULL;

    Ptimeout = dbus_timeout_get_data(timeout);
    ASSERT(Ptimeout != NULL);
    Pret = PyObject_CallMethod(Ptimeout->dcall, "cancel", "");
    Py_XDECREF(Pret);  /* print error Pret == NULL below */
    dbus_timeout_set_data(timeout, NULL, NULL);  /* drops ref to Ptimeout */
    /* fallthrough */

error:
    PRINT_AND_CLEAR_IF_ERROR("remove_timeout_callback()");
}

static void
timeout_toggled_callback(DBusTimeout *timeout, void *data)
{
    int enabled;
    float interval;
    TimeoutObject *Ptimeout;
    PyObject *Pdcall, *Pret = NULL, *Pcallback = NULL;
    PyObject *loop = (PyObject *) data;

    Ptimeout = dbus_timeout_get_data(timeout);
    ASSERT(Ptimeout != NULL);

    /* First disable the current timer... */
    Pret = PyObject_CallMethod(Ptimeout->dcall, "cancel", "");
    if (Pret == NULL)
        RETURN_ERROR(NULL);
    DECREF_SET_NULL(Pret);
    DECREF_SET_NULL(Ptimeout->dcall);

    /* And conditonally create a new one... */
    enabled = dbus_timeout_get_enabled(timeout);
    if (enabled) {
        Pcallback = PyObject_GetAttrString((PyObject *) Ptimeout, "handle");
        if (Pcallback == NULL)
            RETURN_ERROR(NULL);
        interval = (float) dbus_timeout_get_interval(timeout) / 1000.0;
        Pdcall = PyObject_CallMethod(loop, "call_later", "ffO",
                                     interval, interval, Pcallback);
        if (Pdcall == NULL)
            RETURN_ERROR(NULL);
        Ptimeout->dcall = Pdcall;  /* hand off reference */
        DECREF_SET_NULL(Pcallback);
    }

    return;

error:
    Py_XDECREF(Pcallback);
    Py_XDECREF(Pret);
    PRINT_AND_CLEAR_IF_ERROR("timeout_toggled_callback()");
}

static PyObject *
connection_set_loop(ConnectionObject *self, PyObject *args)
{
    PyObject *loop;

    if (!PyArg_ParseTuple(args, "O:set_loop", &loop))
        RETURN_ERROR(NULL);
    if (self->connection == NULL)
        RETURN_ERROR("not connected");
    if (self->loop != NULL)
        RETURN_ERROR("an event loop is already installed");

    if (!PyObject_HasAttrString(loop, "add_reader") ||
                !PyObject_HasAttrString(loop, "remove_reader") ||
                !PyObject_HasAttrString(loop, "add_writer") ||
                !PyObject_HasAttrString(loop, "remove_writer") ||
                !PyObject_HasAttrString(loop, "call_later"))
        RETURN_ERROR("expecting an looping.EventLoop like object");

    self->loop = INCREF(loop);

    if (!dbus_connection_set_watch_functions(self->connection,
            add_watch_callback, remove_watch_callback,
            watch_toggled_callback, self->loop, decref))
        RETURN_ERROR("dbus_connection_set_watch_functions() failed");
    Py_INCREF(self->loop);

    if (!dbus_connection_set_timeout_functions(self->connection,
            add_timeout_callback, remove_timeout_callback,
            timeout_toggled_callback, self->loop, decref))
        RETURN_ERROR("dbus_connection_set_watch_functions() failed");
    Py_INCREF(self->loop);

    Py_RETURN_NONE;

error:
    return NULL;
}


PyDoc_STRVAR(connection_add_filter_doc,
    "add_filter(filter)\n\n"
    "Add a filter handler. The *filter* argument must be a Python callable.\n"
    "The handler will be called with two arguments: the Connection and a\n"
    "Message. It must return a boolean that indicates wether or not the\n"
    "message was accepted.\n\n"
    "Filter handlers are run in the order they were added, and the handler\n"
    "that first accepts a message stops further dispatching.\n\n"
    "Filter handlers are run after method replies are dispatched via the\n"
    "PendingCall mechanism, but before object path handlers.\n");

static PyObject *
connection_add_filter(ConnectionObject *self, PyObject *args)
{
    int found;
    PyObject *filter;

    if (!PyArg_ParseTuple(args, "O:add_filter", &filter))
        RETURN_ERROR(NULL);
    if (!PyCallable_Check(filter))
        RETURN_ERROR("expecting a Python callable");

    if (self->connection == NULL)
        RETURN_ERROR("not connected");
    if ((found = PySet_Contains(self->filters, filter)) < 0)
        RETURN_ERROR(NULL);
    if (!found) {
        if (!dbus_connection_add_filter(self->connection, handler_callback,
                                        filter, decref))
            RETURN_ERROR("dbus_connection_add_filter() failed");
        Py_INCREF(filter);
        if (PySet_Add(self->filters, filter) < 0)
            RETURN_ERROR(NULL);
    }
    Py_RETURN_NONE;

error:
    return NULL;
}


PyDoc_STRVAR(connection_remove_filter_doc,
    "remove_filter(filter)\n\n"
    "Remove a filter that was previously added with :meth:`add_filter`.\n"
    "It is an error to remove a filter that was not added.\n");

static PyObject *
connection_remove_filter(ConnectionObject *self, PyObject *args)
{
    int found;
    PyObject *filter;

    if (!PyArg_ParseTuple(args, "O:remove_filter", &filter))
        RETURN_ERROR(NULL);
    if (!PyCallable_Check(filter))
        RETURN_ERROR("expecting a Python callable");

    if (self->connection == NULL)
        RETURN_ERROR("not connected");
    if ((found = PySet_Contains(self->filters, filter)) < 0)
        RETURN_ERROR(NULL);
    if (!found)
        RETURN_ERROR("no such filter");
    dbus_connection_remove_filter(self->connection, handler_callback, filter);
    if (PySet_Discard(self->filters, filter) < 0)
        RETURN_ERROR(NULL);

    Py_RETURN_NONE;

error:
    return NULL;
}


PyDoc_STRVAR(connection_register_object_path_doc,
    "register_object_path(path, handler, fallback=False)\n\n"
    "Register an object path handler. The *path* argument specifies the\n"
    "path, and *handler* must be a Python callable. If *fallback* is true,\n"
    "any path below the given path will also be handled by the handler\n"
    "It is an error to register an object path handler for a path that\n"
    "was already previously registered.\n\n"
    "The handler will be called with two arguments: the Connection\n"
    "and a Message. It must return a boolean whether or not the message\n"
    "was accepted.\n\n"
    "Object path handlers handle only method calls. Messages will be\n"
    "dispatched to object path handlers after all registered filters\n"
    "have been run. Once a filter or object path handler accepts a message,\n"
    "the message is considered handled and dispatching will stop.\n");

static void
decref_vtable(DBusConnection *connection, void *data)
{
    Py_DECREF((PyObject *) data);
}

static PyObject *
connection_register_object_path(ConnectionObject *self,
                                      PyObject *args)
{
    char *path;
    int ret, fallback = 0;
    PyObject *handler, *Ppath = NULL;
    DBusError error = DBUS_ERROR_INIT;
    DBusObjectPathVTable *vtable = NULL;

    if (!PyArg_ParseTuple(args, "sO|i:register_object_path", &path, &handler,
                          &fallback))
        return NULL;
    if (!_check_path(path))
        RETURN_ERROR("invalid path");
    if (!PyCallable_Check(handler))
        RETURN_ERROR("expecting a Python callable");

    if (self->connection == NULL)
        RETURN_ERROR("not connected");
    if ((vtable = calloc(1, sizeof (DBusObjectPathVTable))) == NULL)
        RETURN_MEMORY_ERROR();
    vtable->message_function = handler_callback;
    vtable->unregister_function = decref_vtable;
    if (fallback)
        ret = dbus_connection_try_register_fallback(self->connection, path,
                            vtable, handler, &error);
    else
        ret = dbus_connection_try_register_object_path(self->connection, path,
                            vtable, handler, &error);
    if (!ret)
        RETURN_ERROR_FROM_DBUS(error);
    Py_INCREF(handler);
    free(vtable); vtable = NULL;
    if ((Ppath = PyUnicode_FromString(path)) == NULL)
        RETURN_ERROR(NULL);
    if (PyDict_SetItem(self->object_paths, Ppath, handler) < 0)
        RETURN_ERROR(NULL);
    Py_DECREF(Ppath); Ppath = NULL;

    Py_RETURN_NONE;

error:
    if (vtable != NULL) free(vtable);
    if (Ppath != NULL) Py_DECREF(Ppath);
    return NULL;
}


PyDoc_STRVAR(connection_unregister_object_path_doc,
    "unregister_object_path(path)\n\n"
    "Unregister an object path handler. The *path* argument specifies\n"
    "the path that will be removed. Both regular as well as fallback\n"
    "handlers can be removed with this call.\n");

static PyObject *
connection_unregister_object_path(ConnectionObject *self,
                                        PyObject *args)
{
    char *path;
    int found;
    PyObject *Ppath = NULL;

    if (!PyArg_ParseTuple(args, "s:unregister_object_path", &path))
        return NULL;
    if (!_check_path(path))
        RETURN_ERROR("invalid path");

    if (self->connection == NULL)
        RETURN_ERROR("not connected");
    if ((Ppath = PyUnicode_FromString(path)) == NULL)
        RETURN_ERROR(NULL);
    if ((found = PyDict_Contains(self->object_paths, Ppath)) < 0)
        RETURN_ERROR(NULL);
    if (!found)
        RETURN_ERROR("no such object path");
    if (!dbus_connection_unregister_object_path(self->connection, path))
        RETURN_ERROR("dbus_connection_unregister_object_path() failed");
    if (PyDict_DelItem(self->object_paths, Ppath) < 0)
        RETURN_ERROR(NULL);
    Py_DECREF(Ppath);

    Py_RETURN_NONE;

error:
    if (Ppath != NULL) Py_DECREF(Ppath);
    return NULL;
}


static PyMethodDef connection_methods[] = \
{
    { "get", (PyCFunction) connection_get,
            METH_VARARGS|METH_KEYWORDS|METH_CLASS, connection_get_doc },
    { "close", (PyCFunction) connection_close, METH_VARARGS,
            connection_close_doc },
    { "send", (PyCFunction) connection_send, METH_VARARGS,
            connection_send_doc },
    { "send_with_reply", (PyCFunction) connection_send_with_reply,
            METH_VARARGS, connection_send_with_reply_doc },
    { "flush", (PyCFunction) connection_flush, METH_VARARGS,
            connection_flush_doc },
    { "dispatch", (PyCFunction) connection_dispatch, METH_VARARGS,
            connection_dispatch_doc },
    { "read_write_dispatch", (PyCFunction) connection_read_write_dispatch,
            METH_VARARGS, connection_read_write_dispatch_doc },
    { "set_loop", (PyCFunction) connection_set_loop, METH_VARARGS,
            connection_set_loop_doc },
    { "add_filter", (PyCFunction) connection_add_filter, METH_VARARGS,
            connection_add_filter_doc },
    { "remove_filter", (PyCFunction) connection_remove_filter,
            METH_VARARGS, connection_remove_filter_doc },
    { "register_object_path", (PyCFunction)
            connection_register_object_path, METH_VARARGS,
            connection_register_object_path_doc },
    { "unregister_object_path", (PyCFunction)
            connection_unregister_object_path, METH_VARARGS,
            connection_unregister_object_path_doc },
    { NULL }
};


static PyObject *
connection_type_init()
{
    ConnectionType.tp_doc = connection_doc;
    ConnectionType.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE \
                                    | Py_TPFLAGS_HAVE_GC ;
    ConnectionType.tp_new = connection_new;
    ConnectionType.tp_init = (initproc) connection_init;
    ConnectionType.tp_dealloc = (destructor) connection_dealloc;
    ConnectionType.tp_traverse = (traverseproc) connection_traverse;
    ConnectionType.tp_clear = (inquiry) connection_clear;
    ConnectionType.tp_methods = connection_methods;
    ConnectionType.tp_getset = connection_properties;
    if (PyType_Ready(&ConnectionType) < 0)
        return NULL; 
    return (PyObject *) &ConnectionType;
}


/**********************************************************************
 * Top-level _dbus module
 */

PyDoc_STRVAR(check_bus_name_doc,
    "check_bus_name(bus_name)\n\n"
    "Return whether or not *bus_name* is a valid D-BUS bus_name.\n");

PyObject *
check_bus_name(PyObject *self, PyObject *args)
{
    char *bus_name;

    if (!PyArg_ParseTuple(args, "s:check_bus_name", &bus_name))
        return NULL;
    return PyBool_FromLong(_check_bus_name(bus_name));
}

PyDoc_STRVAR(check_path_doc,
    "check_path(path)\n\n"
    "Return whether or not *path* is a valid D-BUS path.\n");

PyObject *
check_path(PyObject *self, PyObject *args)
{
    char *path;

    if (!PyArg_ParseTuple(args, "s:check_path", &path))
        return NULL;
    return PyBool_FromLong(_check_path(path));
}

PyDoc_STRVAR(check_interface_doc,
    "check_interface(interface)\n\n"
    "Return whether or not *interface* is a valid D-BUS interface.\n");

PyObject *
check_interface(PyObject *self, PyObject *args)
{
    char *path;

    if (!PyArg_ParseTuple(args, "s:check_path", &path))
        return NULL;
    return PyBool_FromLong(_check_interface(path));
}

PyDoc_STRVAR(check_member_doc,
    "check_member(member)\n\n"
    "Return whether or not *member* is a valid D-BUS member.\n");

PyObject *
check_member(PyObject *self, PyObject *args)
{
    char *path;

    if (!PyArg_ParseTuple(args, "s:check_path", &path))
        return NULL;
    return PyBool_FromLong(_check_member(path));
}

PyDoc_STRVAR(check_error_name_doc,
    "check_error_name(error_name)\n\n"
    "Return whether or not *error_name* is a valid D-BUS error_name.\n");

PyObject *
check_error_name(PyObject *self, PyObject *args)
{
    char *path;

    if (!PyArg_ParseTuple(args, "s:check_error_name", &path))
        return NULL;
    return PyBool_FromLong(_check_interface(path));
}

PyDoc_STRVAR(check_signature_doc,
    "check_signature(signature)\n\n"
    "Return whether or not *signature* is a valid D-BUS signature.\n");

PyObject *
check_signature(PyObject *self, PyObject *args)
{
    char *signature;

    if (!PyArg_ParseTuple(args, "s:check_signature", &signature))
        return NULL;
    return PyBool_FromLong(_check_signature(signature, 0, 0));
}

PyDoc_STRVAR(split_signature_doc,
    "split_signature(signature)\n\n"
    "Split the D-BUS signature in *signature* into a list of complete\n"
    "types.\n");

PyObject *
split_signature(PyObject *self, PyObject *args)
{
    char *signature, *ptr, *end;
    PyObject *Plist, *Pstr = NULL;

    if (!PyArg_ParseTuple(args, "s:split_signature", &signature))
        return NULL;

    if ((Plist = PyList_New(0)) == NULL)
        RETURN_ERROR(NULL);
    ptr = signature;
    while (*ptr != '\000') {
        end = get_one_full_type(ptr);
        if (end == NULL)
            RETURN_ERROR("illegal signature string");
        if ((Pstr = PyUnicode_FromStringAndSize(ptr, end-ptr)) == NULL)
            RETURN_ERROR(NULL);
        if (PyList_Append(Plist, Pstr) < 0)
            RETURN_ERROR(NULL);
        Py_DECREF(Pstr); Pstr = NULL;
        ptr = end;
    }
    return Plist;

error:
    if (Pstr != NULL) Py_DECREF(Pstr);
    return NULL;
}


static PyMethodDef dbus_methods[] = {
    { "check_bus_name", check_bus_name, METH_VARARGS, check_bus_name_doc },
    { "check_path", check_path, METH_VARARGS, check_path_doc },
    { "check_interface", check_interface, METH_VARARGS, check_interface_doc },
    { "check_member", check_member, METH_VARARGS, check_member_doc },
    { "check_error_name", check_error_name, METH_VARARGS, check_error_name_doc },
    { "check_signature", check_signature, METH_VARARGS, check_signature_doc },
    { "split_signature", split_signature, METH_VARARGS, split_signature_doc },
    { NULL }
};

PyDoc_STRVAR(dbus_doc, "Wrapping of the libdbus C API");

MOD_INITFUNC(_dbus)
{
    PyObject *Pmodule, *Pdict, *Pint, *Pstr, *Ptype;

    /* Initialize the module. */

    INIT_MODULE(Pmodule, "_dbus", dbus_doc, dbus_methods);

    if ((Pdict = PyModule_GetDict(Pmodule)) == NULL)
        return MOD_ERROR;
    if ((Error = PyErr_NewExceptionWithDoc("_dbus.Error",
                    "Base class for dbusx exceptions.", NULL, NULL)) == NULL)
        return MOD_ERROR;
    if (PyDict_SetItemString(Pdict, "Error", Error) == -1)
        return MOD_ERROR;

    if (!init_check_number_cache())
        return MOD_ERROR;

    /* NOTE: dbus_threads_init_default() should better use the same thread
     * implementation that Python was compiled with! At least on Linux, Windows
     * and OSX that appears to be the case.
     *
     * The correct solution that works for all platforms would be to register
     * custom thread functions to libdbus that call into Python's threading
     * module. */

    if (!dbus_threads_init_default())
        return MOD_ERROR;

    if (!dbus_connection_allocate_data_slot(&slot_self))
        return MOD_ERROR;

    /* Finalize and export types. */

    if ((Ptype = watch_type_init()) == NULL)
        return MOD_ERROR;
    if ((Ptype = timeout_type_init()) == NULL)
        return MOD_ERROR;
    if ((Ptype = message_type_init()) == NULL)
        return MOD_ERROR;
    if ((PyDict_SetItemString(Pdict, "MessageBase", Ptype) < 0))
        return MOD_ERROR;
    if ((Ptype = connection_type_init()) == NULL)
        return MOD_ERROR;
    if ((PyDict_SetItemString(Pdict, "ConnectionBase", Ptype) < 0))
        return MOD_ERROR;

    /* Add constants. */

    #define EXPORT_INT_SYMBOL(name) \
        do { \
            if ((Pint = PyLong_FromLong(name)) == NULL) return MOD_ERROR; \
            PyDict_SetItemString(Pdict, #name + 5, Pint); \
            Py_DECREF(Pint); \
        } while (0)

    EXPORT_INT_SYMBOL(DBUS_BUS_SYSTEM);
    EXPORT_INT_SYMBOL(DBUS_BUS_SESSION);
    EXPORT_INT_SYMBOL(DBUS_BUS_STARTER);

    EXPORT_INT_SYMBOL(DBUS_MESSAGE_TYPE_INVALID);
    EXPORT_INT_SYMBOL(DBUS_MESSAGE_TYPE_METHOD_CALL);
    EXPORT_INT_SYMBOL(DBUS_MESSAGE_TYPE_METHOD_RETURN);
    EXPORT_INT_SYMBOL(DBUS_MESSAGE_TYPE_ERROR);
    EXPORT_INT_SYMBOL(DBUS_MESSAGE_TYPE_SIGNAL);
    EXPORT_INT_SYMBOL(DBUS_NUM_MESSAGE_TYPES);

    EXPORT_INT_SYMBOL(DBUS_WATCH_READABLE);
    EXPORT_INT_SYMBOL(DBUS_WATCH_WRITABLE);

    EXPORT_INT_SYMBOL(DBUS_DISPATCH_DATA_REMAINS);
    EXPORT_INT_SYMBOL(DBUS_DISPATCH_COMPLETE);
    EXPORT_INT_SYMBOL(DBUS_DISPATCH_NEED_MEMORY);
    
    EXPORT_INT_SYMBOL(DBUS_MAXIMUM_NAME_LENGTH);

    #define EXPORT_STR_SYMBOL(name) \
        do { \
            if ((Pstr = PyUnicode_FromString(name)) == NULL) return MOD_ERROR; \
            PyDict_SetItemString(Pdict, #name + 5, Pstr); \
            Py_DECREF(Pstr); \
        } while (0)

    EXPORT_STR_SYMBOL(DBUS_SERVICE_DBUS);

    EXPORT_STR_SYMBOL(DBUS_PATH_DBUS);
    EXPORT_STR_SYMBOL(DBUS_PATH_LOCAL);

    EXPORT_STR_SYMBOL(DBUS_INTERFACE_DBUS);
    EXPORT_STR_SYMBOL(DBUS_INTERFACE_INTROSPECTABLE);
    EXPORT_STR_SYMBOL(DBUS_INTERFACE_PROPERTIES);
    EXPORT_STR_SYMBOL(DBUS_INTERFACE_PEER);
    EXPORT_STR_SYMBOL(DBUS_INTERFACE_LOCAL);

    EXPORT_STR_SYMBOL(DBUS_ERROR_FAILED);
    EXPORT_STR_SYMBOL(DBUS_ERROR_NO_MEMORY);
    EXPORT_STR_SYMBOL(DBUS_ERROR_SERVICE_UNKNOWN);
    EXPORT_STR_SYMBOL(DBUS_ERROR_NAME_HAS_NO_OWNER);
    EXPORT_STR_SYMBOL(DBUS_ERROR_NO_REPLY);
    EXPORT_STR_SYMBOL(DBUS_ERROR_IO_ERROR);
    EXPORT_STR_SYMBOL(DBUS_ERROR_BAD_ADDRESS);
    EXPORT_STR_SYMBOL(DBUS_ERROR_NOT_SUPPORTED);
    EXPORT_STR_SYMBOL(DBUS_ERROR_LIMITS_EXCEEDED);
    EXPORT_STR_SYMBOL(DBUS_ERROR_ACCESS_DENIED);
    EXPORT_STR_SYMBOL(DBUS_ERROR_AUTH_FAILED);
    EXPORT_STR_SYMBOL(DBUS_ERROR_NO_SERVER);
    EXPORT_STR_SYMBOL(DBUS_ERROR_TIMEOUT);
    EXPORT_STR_SYMBOL(DBUS_ERROR_NO_NETWORK);
    EXPORT_STR_SYMBOL(DBUS_ERROR_ADDRESS_IN_USE);
    EXPORT_STR_SYMBOL(DBUS_ERROR_DISCONNECTED);
    EXPORT_STR_SYMBOL(DBUS_ERROR_INVALID_ARGS);
    EXPORT_STR_SYMBOL(DBUS_ERROR_FILE_NOT_FOUND);
    EXPORT_STR_SYMBOL(DBUS_ERROR_FILE_EXISTS);
    EXPORT_STR_SYMBOL(DBUS_ERROR_UNKNOWN_METHOD);
    EXPORT_STR_SYMBOL(DBUS_ERROR_UNKNOWN_OBJECT);
    EXPORT_STR_SYMBOL(DBUS_ERROR_UNKNOWN_INTERFACE);
    EXPORT_STR_SYMBOL(DBUS_ERROR_UNKNOWN_PROPERTY);
    EXPORT_STR_SYMBOL(DBUS_ERROR_PROPERTY_READ_ONLY);
    EXPORT_STR_SYMBOL(DBUS_ERROR_TIMED_OUT);
    EXPORT_STR_SYMBOL(DBUS_ERROR_MATCH_RULE_NOT_FOUND);
    EXPORT_STR_SYMBOL(DBUS_ERROR_MATCH_RULE_INVALID);
    EXPORT_STR_SYMBOL(DBUS_ERROR_SPAWN_EXEC_FAILED);
    EXPORT_STR_SYMBOL(DBUS_ERROR_SPAWN_FORK_FAILED);
    EXPORT_STR_SYMBOL(DBUS_ERROR_SPAWN_CHILD_EXITED);
    EXPORT_STR_SYMBOL(DBUS_ERROR_SPAWN_CHILD_SIGNALED);
    EXPORT_STR_SYMBOL(DBUS_ERROR_SPAWN_FAILED);
    EXPORT_STR_SYMBOL(DBUS_ERROR_SPAWN_SETUP_FAILED);
    EXPORT_STR_SYMBOL(DBUS_ERROR_SPAWN_CONFIG_INVALID);
    EXPORT_STR_SYMBOL(DBUS_ERROR_SPAWN_SERVICE_INVALID);
    EXPORT_STR_SYMBOL(DBUS_ERROR_SPAWN_SERVICE_NOT_FOUND);
    EXPORT_STR_SYMBOL(DBUS_ERROR_SPAWN_PERMISSIONS_INVALID);
    EXPORT_STR_SYMBOL(DBUS_ERROR_SPAWN_FILE_INVALID);
    EXPORT_STR_SYMBOL(DBUS_ERROR_SPAWN_NO_MEMORY);
    EXPORT_STR_SYMBOL(DBUS_ERROR_UNIX_PROCESS_ID_UNKNOWN);
    EXPORT_STR_SYMBOL(DBUS_ERROR_INVALID_SIGNATURE);
    EXPORT_STR_SYMBOL(DBUS_ERROR_INVALID_FILE_CONTENT);
    EXPORT_STR_SYMBOL(DBUS_ERROR_SELINUX_SECURITY_CONTEXT_UNKNOWN);
    EXPORT_STR_SYMBOL(DBUS_ERROR_ADT_AUDIT_DATA_UNKNOWN);
    EXPORT_STR_SYMBOL(DBUS_ERROR_OBJECT_PATH_IN_USE);
    EXPORT_STR_SYMBOL(DBUS_ERROR_INCONSISTENT_MESSAGE);

    EXPORT_STR_SYMBOL(DBUS_INTROSPECT_1_0_XML_NAMESPACE);
    EXPORT_STR_SYMBOL(DBUS_INTROSPECT_1_0_XML_PUBLIC_IDENTIFIER);
    EXPORT_STR_SYMBOL(DBUS_INTROSPECT_1_0_XML_SYSTEM_IDENTIFIER);
    EXPORT_STR_SYMBOL(DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE);

    return MOD_OK(Pmodule);
}
