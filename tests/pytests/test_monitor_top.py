# =============================================================================
# 2015+ Copyright (c) Budnik Andrey <budnik27@gmail.com>
# All rights reserved.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# =============================================================================

import sys
sys.path.insert(0, "")  # for running from cmake
from conftest import make_session
import pytest
import elliptics
import zlib
import json
import hashlib
try:
    import urllib.request as urllib_req
except ImportError:
    import urllib2 as urllib_req


def get_top_by_http(remote, monitor_port):
    '''get json document, containing list of top keys via http'''
    url = 'http://' + remote + ':' + monitor_port + '/top'
    data = urllib_req.urlopen(url).read()
    json_data = zlib.decompress(data)
    '''convert json to python dict'''
    return json.loads(json_data)

def has_key(key, keys):
    for k in keys:
        if k['id'] == key:
            return True
    return False

def check_key_fields(keys):
    '''all items in top key list must contain group_id, key_id, traffic size generated by key access, key access frequency'''
    for k in keys:
        assert k['group']
        assert k['id']
        assert k['size']
        assert k['frequency']

def check_response_fields(response):
    '''top object must contain top_result_limit, period_in_seconds fields'''
    assert response['top']['top_result_limit']
    assert response['top']['period_in_seconds']

class TestMonitorTop:
    '''
    This test suite does single write to a server and then checks that key of this write exists among top keys,
    using http and elliptics protocol.
    It checks that top keys service exists and items of top keys list contains all required fields.
    '''

    def test_single_write(self, server, simple_node):
        '''
        check that single access to some key appears among top keys using http interface of monitoring service
        '''
        session = make_session(node=simple_node,
                               test_name='TestMonitorTop.test_single_write')
        groups = session.routes.groups()
        session.groups = groups

        test_key = 'one_key'
        session.write_data(test_key, 'some_data').get()
        session.read_latest(test_key).get()

        test_key = hashlib.sha512(test_key).hexdigest()
        top_keys = []
        for remote, port in zip(server.remotes, server.monitors):
            remote = remote.split(':')[0]
            response = get_top_by_http(remote, port)
            '''check that response contains required fields'''
            check_response_fields(response)
            top_keys = response['top']['top_by_size']
            if has_key(test_key, top_keys):
                break
        '''check that written key appears among top keys'''
        assert has_key(test_key, top_keys)
        '''check that all top keys items contains all required fields'''
        check_key_fields(top_keys)

        self.__check_key_existance_using_session_monitor(session, test_key)

    def __check_key_existance_using_session_monitor(self, session, test_key):
        '''
        check that access to single key appears in monitor top using elliptics protocol
        '''
        entries = session.monitor_stat(categories=elliptics.core.monitor_stat_categories.top).get()
        top_keys = []
        for entry in entries:
            response = entry.statistics
            '''check that response contains required fields'''
            check_response_fields(response)
            top_keys = response['top']['top_by_size']
            if has_key(test_key, top_keys):
                break
        '''check that written key appears among top keys'''
        assert has_key(test_key, top_keys)
