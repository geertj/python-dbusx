#
# This file is part of python-dbusx. Python-dbusx is free software
# available under the terms of the MIT license. See the file "LICENSE" that
# was provided together with this source file for the licensing terms.
#
# Copyright (c) 2012-2013 the python-dbusx authors. See the file "AUTHORS"
# for a complete list.

import dbusx
from dbusx.test import assert_raises


class TestSplitSignature(object):

    def test_empty(self):
        assert dbusx.split_signature('') == []

    def test_single(self):
        assert dbusx.split_signature('i') == ['i']

    def test_multiple(self):
        assert dbusx.split_signature('iu') == ['i', 'u']

    def test_array(self):
        assert dbusx.split_signature('aiu') == ['ai', 'u']

    def test_nested_array(self):
        assert dbusx.split_signature('aaiu') == ['aai', 'u']

    def test_struct(self):
        assert dbusx.split_signature('(ii)u') == ['(ii)', 'u']

    def test_dict_entry(self):
        assert dbusx.split_signature('{ss}u') == ['{ss}', 'u']
    

def nested_sig(narray, nstruct, typ):
    return 'a' * narray + '(' * nstruct + typ + ')' * nstruct


class TestCheckSignature(object):

    def test_check_signature(self):
        assert dbusx.check_signature('i')
        assert dbusx.check_signature('ii')
        assert dbusx.check_signature('(ii)')
        assert dbusx.check_signature('{ss}')

    def test_maximally_nested(self):
        assert dbusx.check_signature(nested_sig(32, 32, 'i'))

    def test_check_unknown_type(self):
        assert not dbusx.check_signature('_')
        assert not dbusx.check_signature('I')

    def test_array_without_type(self):
        assert not dbusx.check_signature('a')
        assert not dbusx.check_signature('aa')

    def test_signature_unbalanced_parentheses(self):
        assert not dbusx.check_signature('i)')
        assert not dbusx.check_signature('((i)')
        assert not dbusx.check_signature('{i')
        assert not dbusx.check_signature('i}')
        assert not dbusx.check_signature('{{i')
        assert not dbusx.check_signature('(i')

    def test_nested_too_much(self):
        assert not dbusx.check_signature(nested_sig(32, 33, 'i'))
        assert not dbusx.check_signature(nested_sig(33, 32, 'i'))
        assert not dbusx.check_signature(nested_sig(33, 33, 'i'))
