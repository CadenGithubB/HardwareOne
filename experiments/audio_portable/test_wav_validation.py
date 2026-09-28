#!/usr/bin/env python3
"""Check acoustic qualification rejects silent, misclocked and truncated audio."""
import array
import base64
import json
import re
import shlex
from pathlib import Path
import math
import sys
import tempfile
import unittest
import wave
from test_hardware import (inspect_wav, file_chunk_pattern, parse_file_chunk, read_owned_file,
                           ConsoleTimeout, ConnectivityConsoleFatal)


class WaveValidation(unittest.TestCase):
    def fixture(self, folder, rate=16000, frequency=997, duration=.8, value=None):
        path=Path(folder)/'test.wav'
        samples=array.array('h',(int(4000*math.sin(2*math.pi*frequency*i/rate)) if value is None else value for i in range(int(rate*duration))))
        if sys.byteorder!='little': samples.byteswap()
        with wave.open(str(path),'wb') as f:
            f.setnchannels(1);f.setsampwidth(2);f.setframerate(rate);f.writeframes(samples.tobytes())
        return path

    def test_supported_reference_rates(self):
        with tempfile.TemporaryDirectory() as folder:
            for rate in (8000,16000,48000):
                with self.subTest(rate=rate):
                    value=inspect_wav(self.fixture(folder,rate),rate,1.1,True,.7)
                    self.assertLessEqual(abs(value['reference_tone']['frequency_hz']-997),8)

    def test_silence_clipping_wrong_clock_and_sample_loss(self):
        with tempfile.TemporaryDirectory() as folder:
            for options in ({'value':0},{'value':32767},{'frequency':1994},{'duration':.3}):
                with self.subTest(options=options), self.assertRaises(ValueError):
                    inspect_wav(self.fixture(folder,**options),16000,1.1,True,.8)

    def test_header_and_truncation(self):
        with tempfile.TemporaryDirectory() as folder:
            path=self.fixture(folder)
            with self.assertRaises(ValueError): inspect_wav(path,48000,1.1)
            path.write_bytes(path.read_bytes()[:-100])
            with self.assertRaises(ValueError): inspect_wav(path,16000,1.1)


TEST_PATH = '/recordings/rec_1234567812345678.wav'


def envelope(payload, offset=0, total=None, path=TEST_PATH, **changes):
    value = {'success':True, 'path':path, 'size':len(payload) if total is None else total,
             'offset':offset, 'len':len(payload),
             'eof':offset+len(payload) == (len(payload) if total is None else total),
             'enc':'b64', 'data':base64.b64encode(payload).decode('ascii')}
    value.update(changes)
    return json.dumps(value, separators=(',', ':'))


class FileChunkValidation(unittest.TestCase):
    def setUp(self):
        self.payload = bytes(range(256))*2
        self.good = envelope(self.payload, total=1024)

    def test_exact_identity_and_stale_envelopes(self):
        other = envelope(self.payload, total=1024, path=TEST_PATH.replace('12345678', '87654321'))
        stale = envelope(self.payload, offset=512, total=1024)
        self.assertEqual(parse_file_chunk(other+'\n'+stale+'\n'+self.good, TEST_PATH, 0), (1024,self.payload))
        for response in (other, stale, '$ ', 'You are test (admin)\n'):
            with self.subTest(response=response[:40]), self.assertRaises(ValueError):
                parse_file_chunk(response, TEST_PATH, 0)

    def test_truncation_base64_lengths_ranges_and_eof(self):
        responses = (
            self.good[:-1],
            envelope(self.payload,total=1024,data='!invalid!'),
            envelope(self.payload,total=1024,data=base64.b64encode(self.payload).decode('ascii')[:-1]),
            envelope(self.payload,total=1024,len=511),
            envelope(self.payload[:511],total=1024),
            envelope(self.payload,total=1024,eof=True),
            envelope(self.payload,total=1024,enc='raw'),
            envelope(self.payload,total=40),
            envelope(self.payload,total=1024*1024+1),
            envelope(self.payload,total=1024,offset=True),
        )
        for response in responses:
            with self.subTest(response=response[:100]), self.assertRaises(ValueError):
                parse_file_chunk(response, TEST_PATH, 0)
        with self.assertRaises(ValueError):
            parse_file_chunk(self.good, TEST_PATH, 0, total=1025)
        with self.assertRaises(ValueError):
            parse_file_chunk(envelope(self.payload,total=512,offset=512),TEST_PATH,512)
        # Python accepts nonzero unused padding bits; canonical re-encoding must reject them.
        with self.assertRaises(ValueError):
            parse_file_chunk(envelope(b'\xff',offset=512,total=513,data='/x=='),TEST_PATH,512,total=513)

    def test_eof_and_consistent_duplicate_reads(self):
        tail = envelope(b'abc',offset=512,total=515)
        self.assertEqual(parse_file_chunk(tail,TEST_PATH,512,total=515), (515,b'abc'))
        self.assertEqual(parse_file_chunk(self.good+'\n'+self.good,TEST_PATH,0), (1024,self.payload))
        different = envelope(b'x'*512,total=1024)
        with self.assertRaises(ValueError): parse_file_chunk(self.good+'\n'+different,TEST_PATH,0)

    def test_patterns_reject_unowned_paths_and_correlate_complete_envelopes(self):
        pattern=file_chunk_pattern(TEST_PATH,0)
        self.assertIsNotNone(pattern.search(self.good))
        self.assertIsNone(pattern.search(self.good[:-1]))
        self.assertIsNone(pattern.search(envelope(self.payload,total=1024,offset=10)))
        for path in ('/recordings/../settings.json','/settings.json','/recordings/rec_old.wav'):
            with self.subTest(path=path), self.assertRaises(ValueError): file_chunk_pattern(path,0)


class ReadOnlyConsole:
    """Exercise the real reader orchestration without opening a serial device."""
    def __init__(self, data, actions=()):
        self.data=data; self.actions=list(actions); self.sent=[]; self.fatal_console=False
        self.offset=0; self.length=0
    def send_line(self, command):
        parts=shlex.split(command)
        assert len(parts)==5 and parts[0]=='fileread' and parts[1]==TEST_PATH and parts[4]=='b64'
        self.offset=int(parts[2]); self.length=int(parts[3]); self.sent.append(command)
        return len(self.sent)
    def read_until(self, pattern, *, since, timeout):
        assert since==len(self.sent) and timeout==10
        response=envelope(self.data[self.offset:self.offset+self.length],offset=self.offset,total=len(self.data))
        action=self.actions.pop(0) if self.actions else 'good'
        if action=='malformed':
            value=json.loads(response);value['data']='!bad!';response=json.dumps(value,separators=(',',':'))
        elif action=='truncated': response=response[:-3]
        elif action=='stale':
            value=json.loads(response);value['offset']+=512;response=json.dumps(value,separators=(',',':'))
        elif action=='timeout': response=''
        if not pattern.search(response): raise ConsoleTimeout(since,timeout,response)
        return response


class ReadOnlyTransfer(unittest.TestCase):
    def setUp(self):
        self.data=bytes(range(251))*5
        self.retries=0;self.health_calls=0;self.waits=[]
    def reread(self): self.retries+=1
    def health(self): self.health_calls+=1
    def read(self, console):
        return read_owned_file(console,TEST_PATH,self.health,self.reread,wait=self.waits.append)

    def test_512_byte_transfer_and_bounded_same_offset_recovery(self):
        board=ReadOnlyConsole(self.data, ('truncated','malformed','stale','good'))
        self.assertEqual(self.read(board),self.data)
        self.assertEqual(self.retries,3)
        offsets=[int(shlex.split(command)[2]) for command in board.sent]
        self.assertEqual(offsets,[0,0,0,0,512,1024])
        self.assertTrue(all(shlex.split(command)[3]=='512' for command in board.sent))
        self.assertEqual(self.waits,[.08]*len(board.sent))
        self.assertEqual(self.health_calls,2*len(board.sent))

    def test_four_attempt_limit(self):
        for failure,exception in (('timeout',ConsoleTimeout),('malformed',ValueError)):
            with self.subTest(failure=failure):
                self.retries=0
                board=ReadOnlyConsole(self.data, (failure,)*4)
                with self.assertRaises(exception): self.read(board)
                self.assertEqual(len(board.sent),4)
                self.assertEqual(self.retries,3)

    def test_fatal_guards_never_retry(self):
        board=ReadOnlyConsole(self.data);board.fatal_console=True
        with self.assertRaises(ConnectivityConsoleFatal): self.read(board)
        self.assertEqual(board.sent,[]);self.assertEqual(self.retries,0)
        board=ReadOnlyConsole(self.data)
        def panic_after_read():
            if board.sent: raise ValueError('Fatal board log')
        with self.assertRaisesRegex(ValueError,'Fatal board log'):
            read_owned_file(board,TEST_PATH,panic_after_read,self.reread,wait=self.waits.append)
        self.assertEqual(len(board.sent),1);self.assertEqual(self.retries,0)


if __name__=='__main__': unittest.main()
