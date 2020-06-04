#!/usr/bin/env python3

import argparse
import sys
import shlex
import json
import socket
import time
import os
import copy

try:
    from shlex import quote
except ImportError:
    from pipes import quote


class JsonRpcSnapException(Exception):
    def __init__(self, message):
        self.message = message


class JsonRpcSnapClient(object):
    decoder = json.JSONDecoder()

    def __init__(self, sockpath, timeout=60.0):
        self.sock = None
        self._request_id = 0
        self.timeout = timeout
        try:
            if os.path.exists(sockpath):
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.connect(sockpath)
            else:
                raise socket.error("Unix socket '%s' does not exist" % sockpath)
        except socket.error as ex:
            raise JsonRpcSnapException("Error while connecting to %s\n"
                                       "Error details: %s" % (sockpath, ex))

    def __json_to_string(self, request):
        return json.dumps(request)

    def send(self, method, params=None):
        self._request_id += 1
        req = {
            'jsonrpc': '2.0',
            'method': method,
            'id': self._request_id
        }
        if params:
            req['params'] = copy.deepcopy(params)

        self.sock.sendall(self.__json_to_string(req).encode("utf-8"))
        return self._request_id

    def __string_to_json(self, request_str):
        try:
            obj, idx = self.decoder.raw_decode(request_str)
            return obj
        except ValueError:
            return None

    def recv(self):
        timeout = self.timeout
        start_time = time.process_time()
        response = None
        buf = ""

        while not response:
            try:
                timeout = timeout - (time.process_time() - start_time)
                self.sock.settimeout(timeout)
                buf += self.sock.recv(4096).decode("utf-8")
                response = self.__string_to_json(buf)
            except socket.timeout:
                break
            except ValueError:
                continue  # incomplete response; keep buffering

        if not response:
            raise JsonRpcSnapException("Response Timeout")
        return response

    def call(self, method, params={}):
        req_id = self.send(method, params)
        response = self.recv()

        if 'error' in response:
            params["method"] = method
            params["req_id"] = req_id
            msg = "\n".join(["request:", "%s" % json.dumps(params, indent=2),
                             "Got JSON-RPC error response",
                             "response:",
                             json.dumps(response['error'], indent=2)])
            raise JsonRpcSnapException(msg)

        return response['result']


def main():
    parser = argparse.ArgumentParser(
        description='Mellanox SNAP JSON-RPC 2.0 command line interface')
    parser.add_argument('-s', dest='server_addr',
                        help='RPC domain socket path',
                        default='/var/tmp/spdk.sock')
    parser.add_argument('-t', dest='timeout',
                        help='Timeout as a floating point number expressed in '
                             'seconds waiting for response. Default: 60.0',
                        default=60.0, type=float)
    parser.add_argument('-v', dest='verbose',
                        choices=['DEBUG', 'INFO', 'ERROR'],
                        help="""Set verbosity level. """)
    subparsers = parser.add_subparsers(help='Mellanox SNAP JSON-RPC 2.0 Client methods',
                                       dest='called_rpc_name')

    def controller_nvme_delete(args):
        params = {
            'name': args.name,
        }
        args.client.call('controller_nvme_delete', params)
    p = subparsers.add_parser('controller_nvme_delete',
                              help='Destroy NVMe SNAP controller')
    p.add_argument('name', help='Controller Name', type=str)
    p.set_defaults(func=controller_nvme_delete)

    def controller_nvme_create(args):
        params = {
            'pci_func': args.pci_func,
        }
        if args.conf:
            params['conf_file'] = args.conf

        result = args.client.call('controller_nvme_create', params)
        print(json.dumps(result, indent=2).strip('"'))
    p = subparsers.add_parser('controller_nvme_create',
                              help='Create new NVMe SNAP controller')
    p.add_argument('pci_func', help='PCI function to start emulation on',
                   type=int)
    p.add_argument('-c', '--conf', help='JSON configuration file to use',
                   type=str, required=False)
    p.set_defaults(func=controller_nvme_create)

    def emulation_list(args):
        result = args.client.call('emulation_list')
        print(json.dumps(result, indent=2))
    __help = 'List all SNAP plugged emulation functions with their characteristics'
    p = subparsers.add_parser('emulation_list', help=__help)
    p.set_defaults(func=emulation_list)

    def call_rpc_func(args):
        args.func(args)

    def execute_script(parser, client, fd):
        executed_rpc = ""
        for rpc_call in map(str.rstrip, fd):
            if not rpc_call.strip():
                continue
            executed_rpc = "\n".join([executed_rpc, rpc_call])
            args = parser.parse_args(shlex.split(rpc_call))
            args.client = client
            try:
                call_rpc_func(args)
            except JsonRpcSnapException as ex:
                print("Exception:")
                print(executed_rpc.strip() + " <<<")
                print(ex.message)
                exit(1)

    args = parser.parse_args()
    args.client = JsonRpcSnapClient(args.server_addr, args.timeout)
    if hasattr(args, 'func'):
        try:
            call_rpc_func(args)
        except JsonRpcSnapException as ex:
            print(ex)
            exit(1)
    elif sys.stdin.isatty():
        # No arguments and no data piped through stdin
        parser.print_help()
        exit(1)
    else:
        execute_script(parser, args.client, sys.stdin)


if __name__ == "__main__":
    main()
