"""Rejected duplicate handshakes must not evict an accepted driver."""
import json
import select
import socket
import struct
import sys
import time
import os
import signal
import subprocess
import tempfile
from smoke_handshake import TCP_PORT, write_cfg
from pathlib import Path

import fake_client as client

channels = []

class Channel:
    def __init__(self):
        self.sock = socket.create_connection(('127.0.0.1', TCP_PORT),timeout=3)
        self.buffer = b''
        self.events = []
        self.closed = False
        channels.append(self)

    def send(self, body):
        self.sock.sendall(struct.pack('<H',len(body))+body)

    def close(self):
        self.closed = True
        self.sock.close()

def pump(seconds=.05):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        active=[c for c in channels if not c.closed]
        if not active:
            return
        ready,_,_=select.select([c.sock for c in active],[],[],max(0,end-time.monotonic()))
        for c in active:
            if c.sock not in ready:
                continue
            try:
                chunk=c.sock.recv(65536)
            except ConnectionResetError:
                chunk=b''
            if not chunk:
                c.close();continue
            c.buffer+=chunk
            while len(c.buffer)>=2:
                size=struct.unpack_from('<H',c.buffer)[0]
                if len(c.buffer)<size+2:
                    break
                c.events.append(c.buffer[2:size+2]);c.buffer=c.buffer[size+2:]
            c.events=c.events[-1000:]

def until(condition,seconds=4):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        if condition():
            return
        pump(.05)
    assert condition(),'Timed out waiting for expected protocol state'

def driver(index):
    c=Channel()
    steam='S'+str(76561199010000000+index)
    body=bytes([9])+struct.pack('<H',0x100)+client.wstr_a('')+client.build_driver_info(steam=steam)+bytes(8)+client.build_car_info(race_number=100+index)
    c.send(body)
    until(lambda: c.closed or any(p[0] in (0x0b,0x0c) for p in c.events))
    return c,steam

def welcome(c):
    packets=[p for p in c.events if p[0]==0x0b]
    assert packets,('No welcome',[(p[0],p[:8].hex()) for p in c.events],c.closed)
    client.parse_welcome(packets[-1])
    return packets[-1]

def rejected(c):
    until(lambda: c.closed or any(p[0]==0x0c for p in c.events))
    assert any(p[0]==0x0c and p[1]==9 for p in c.events),'Expected explicit server-full response'

def monitor():
    c=Channel()
    c.send(b'\x0a\x05probe\x10\xfa\x01\x18\x01\x20\x01\x28\x01')
    until(lambda: any(p[0]==1 for p in c.events) or c.closed)
    assert any(p[:3]==b'\x01\x08\x01' for p in c.events),'Monitor rejected'
    return c

def varint(data,pos):
    value=shift=0
    while True:
        n=data[pos];pos+=1;value|=(n&127)<<shift
        if n<128:return value,pos
        shift+=7

def protobuf(data):
    out={};pos=0
    while pos<len(data):
        tag,pos=varint(data,pos);wire=tag&7
        if wire==0:value,pos=varint(data,pos)
        elif wire==2:
            size,pos=varint(data,pos);value=data[pos:pos+size];pos+=size
        elif wire in (1,5):
            size=8 if wire==1 else 4;value=data[pos:pos+size];pos+=size
        else:raise AssertionError('Unsupported protobuf wire type')
        out.setdefault(tag>>3,[]).append(value)
    return out

def monitor_count(m):
    for packet in reversed(m.events):
        if packet[0]==6:
            session=protobuf(packet[1:]).get(2,[])
            if session:return protobuf(session[0]).get(13,[0])[0]
    return None

def run():
    executable = Path(__file__).resolve().parent.parent / 'accd'
    with tempfile.TemporaryDirectory(prefix='accd-duplicate-') as tmp:
        write_cfg(tmp)
        settings=Path(tmp)/'settings.json'
        data=json.loads(settings.read_text())
        data['password']='test-secret'
        settings.write_text(json.dumps(data))
        proc=subprocess.Popen([str(executable),tmp],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True)
        try:
            deadline=time.monotonic()+4
            while True:
                try:
                    with socket.create_connection(('127.0.0.1',TCP_PORT),timeout=.2):break
                except OSError:
                    if time.monotonic()>=deadline:raise RuntimeError('server did not start')
                    time.sleep(.05)
            def connect(password):
                c=Channel()
                steam='S76561199010000123'
                payload=(bytes([9])+struct.pack('<H',0x100)+client.wstr_a(password)+
                         client.build_driver_info(steam=steam)+bytes(8)+client.build_car_info(race_number=123))
                c.send(payload)
                until(lambda:c.closed or any(p[0] in (0x0b,0x0c) for p in c.events))
                return c
            m=monitor()
            first=connect('test-secret');welcome(first);until(lambda:monitor_count(m)==1)
            invalid=connect('wrong-secret')
            assert any(p[0]==0x0c and p[1]==6 for p in invalid.events), 'wrong password not rejected'
            pump(.4)
            assert not first.closed and monitor_count(m)==1, 'rejected handshake evicted the existing driver'
            reconnect=connect('test-secret');welcome(reconnect)
            until(lambda:first.closed and monitor_count(m)==1)
            print('PASS: rejected duplicate preserves driver and valid reconnect succeeds')
        finally:
            for c in channels:
                if not c.closed:c.close()
            channels.clear()
            try:
                os.killpg(proc.pid,signal.SIGTERM);proc.wait(timeout=2)
            except (ProcessLookupError,subprocess.TimeoutExpired):
                try:os.killpg(proc.pid,signal.SIGKILL)
                except ProcessLookupError:pass

if __name__=='__main__':
    run()
