"""Verify optional monitor capacity independently of driver slots."""
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
from smoke_handshake import TCP_PORT, UDP_PORT, write_cfg
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

def scenario(extra):
    executable = Path(__file__).resolve().parent.parent / 'accd'
    with tempfile.TemporaryDirectory(prefix='accd-monitor-capacity-') as tmp:
        write_cfg(tmp)
        cfg=Path(tmp)
        configuration=json.loads((cfg/'configuration.json').read_text())
        configuration['maxConnections']=10
        (cfg/'configuration.json').write_text(json.dumps(configuration))
        settings=json.loads((cfg/'settings.json').read_text())
        settings.update(maxCarSlots=10, maxMonitors=1, maxExtraMonitorConnections=extra)
        (cfg/'settings.json').write_text(json.dumps(settings))
        proc=subprocess.Popen([str(executable),tmp],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True)
        try:
            deadline=time.monotonic()+4
            while True:
                try:
                    with socket.create_connection(('127.0.0.1',TCP_PORT),timeout=.2): break
                except OSError:
                    if time.monotonic()>=deadline:raise RuntimeError('server did not start')
                    time.sleep(.05)
            m=monitor()
            drivers=[]
            for index in range(1,10):
                c,_=driver(index);welcome(c);drivers.append(c)
            if not extra:
                c,_=driver(10);rejected(c)
                print('PASS: default shared connection limit remains unchanged')
            else:
                c,_=driver(10);welcome(c);drivers.append(c)
                until(lambda:monitor_count(m)==10)
                assert all(not c.closed for c in drivers)
                overflow,_=driver(11);rejected(overflow)
                m.close();pump(.2)
                m=monitor();until(lambda:monitor_count(m)==10)
                assert all(not c.closed for c in drivers)
                print('PASS: extra monitor slot allows ten drivers, rejects overflow and reconnects monitor')
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
    scenario(0)
    scenario(1)
