#!/usr/bin/env python

"""
A simple echo server
"""

import socket

host = ''
port = 41415
backlog = 5
size = 1024
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.bind((host,port))
s.listen(backlog)
while 1:
    client, address = s.accept()
    data = client.recv(size)
    if data:
        client.send(data)
    client.shutdown()
    client.close()
