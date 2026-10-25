#!/usr/bin/env python3
"""Execute the production metadata load block with real ArduinoJson."""
import argparse
from pathlib import Path
import os
import subprocess
import tempfile
from test_web_batch_handlers import extract_block
HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[1]
p=argparse.ArgumentParser();p.add_argument('--sanitize',action='store_true');args=p.parse_args()
source=(ROOT/'BLE_Peers.cpp').read_text();read=extract_block(source,'void blePeersReadJson(')
block=extract_block(read,'if (row.kind == BLE_PEER_PHONE)')
unit=r'''
#include <ArduinoJson.h>
#include <cassert>
#include <string>
#include <cstdio>
constexpr int BLE_PEER_PHONE=2;
static struct {std::string pipeEnrollment;}gBlePeerData[3];
static void load(const char* text){JsonDocument doc;assert(!deserializeJson(doc,text));JsonObjectConst e=doc.as<JsonObjectConst>();struct {int kind=BLE_PEER_PHONE;}row;
// BLOCK
}
int main(){
  for(const char* json:{"{}","{\"pipeBondVersion\":1,\"pipeIdentity\":\"C2:00:00:00:00:02\",\"pipeIdentityType\":1}","{\"mac1\":\"C2:00:00:00:00:02\"}","{\"pipeChannelVersion\":1}","{\"pipeChannelVersion\":1,\"pipeEnrolled\":false}","{\"pipeChannelVersion\":1,\"pipeEnrolled\":1}","{\"pipeChannelVersion\":\"1\",\"pipeEnrolled\":true}","{\"pipeChannelVersion\":2,\"pipeEnrolled\":true}"}){
    gBlePeerData[2].pipeEnrollment="old";load(json);assert(gBlePeerData[2].pipeEnrollment.empty());
  }
  load("{\"pipeChannelVersion\":1,\"pipeEnrolled\":true}");assert(gBlePeerData[2].pipeEnrollment=="app-key-v1");
  puts("Phone enrollment schema: old SMP/console metadata and wrong types/version rejected; explicit v1 marker accepted");
}
'''.replace('// BLOCK',block)
with tempfile.TemporaryDirectory(prefix='hw1-pipe-schema-') as tmp:
 d=Path(tmp);(d/'test.cpp').write_text(unit)
 cmd=[os.environ.get('CXX','clang++'),'-std=c++17','-Wall','-Wextra','-Werror','-I',str(ROOT.parent/'hardwareone_libs/ArduinoJson/src'),str(d/'test.cpp'),'-o',str(d/'test')]
 if args.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']
 subprocess.run(cmd,check=True);subprocess.run([str(d/'test')],check=True)
