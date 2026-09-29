#!/usr/bin/env python3
"""Exercise actual passive STT command/HTTP activity classifiers, without firmware dependencies."""
import argparse
from pathlib import Path
import subprocess
import tempfile
from test_espsr_runtime import function
HERE=Path(__file__).resolve().parent
COMPONENT=HERE.parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--sanitize',action='store_true');a=p.parse_args()
    commands=(COMPONENT/'System_Utils.cpp').read_text()
    server=(COMPONENT/'WebServer_Server.cpp').read_text()
    unit='''#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
enum { HTTP_GET, HTTP_POST, HTTP_PUT };
struct httpd_req_t { int method; };
'''
    unit+='\n'.join(function(commands,s) for s in ('static bool isPassiveTranscriptionCommand(', 'static bool isQuietPollCommand('))
    unit+='\n'+function(server,'static bool requestIsInteraction(')
    unit+='''
int main() {
 for (const char* command : {"transcription status", "transcription status 0123456789abcdef", "transcription next 0123456789abcdef", "transcription ack 0123456789abcdef 9 0 13", "transcription ack\\t0123456789abcdef 9 0 13"}) {
  assert(isPassiveTranscriptionCommand(command)); assert(isQuietPollCommand(command));
 }
 for (const char* command : {"", "transcription", "transcription start", "transcription stop 0123456789abcdef", "transcription cancel 0123456789abcdef", "transcription statusx", "transcription next-file", "transcription acknowledgment", "transcription ack; reboot", "other transcription status", "sttsavetranscripts on", "transcripts read \\\"/stt/u7/file.txt\\\" 0"}) {
  assert(!isPassiveTranscriptionCommand(command)); assert(!isQuietPollCommand(command));
 }
 assert(!isPassiveTranscriptionCommand(nullptr)); assert(!isQuietPollCommand(nullptr));
 assert(isQuietPollCommand("g2status")); assert(isQuietPollCommand("cm5 status"));
 httpd_req_t post{HTTP_POST},get{HTTP_GET},put{HTTP_PUT};
 for (const char* uri : {"/api/transcription/ack", "/api/transcription/ack?receipt=9", "/api/transcription/ack?"}) assert(!requestIsInteraction(&post,uri));
 for (const char* uri : {"/api/transcription", "/api/transcription?action=start", "/api/transcription/ack-extra", "/api/transcription/ack/", "/api/transcription/acknowledge", "/api/cli", "/api/cli?cmd=transcription+start"}) assert(requestIsInteraction(&post,uri));
 assert(!requestIsInteraction(&post,"/api/cli/batch"));
 assert(!requestIsInteraction(&get,"/api/transcription"));
 assert(!requestIsInteraction(&get,"/api/transcription?identity=1"));
 assert(requestIsInteraction(&get,"/sensors"));
 assert(!requestIsInteraction(&put,"/api/transcription"));
 assert(!requestIsInteraction(nullptr,"/api/transcription")); assert(!requestIsInteraction(&post,nullptr));
 puts("Transcription activity: automatic status/next/ACK stay passive; user controls and near-match routes remain interactions");
}
'''
    execution=function(commands,'bool executeCommand(AuthContext& ctx, const char* cmd, char* out, size_t outSize)')
    before_activity=execution[:execution.index('powerSaveNoteActivity();')]
    assert '!isPassiveTranscriptionCommand(cmd)' in before_activity, 'Classifier must gate the actual activity call'
    assert 'isQuietPollCommand(cmd)' in function(commands,'static bool commandExecutionLogSuppressed(')
    with tempfile.TemporaryDirectory(prefix='hw1-transcription-activity-') as t:
        cpp=Path(t)/'test.cpp';exe=Path(t)/'test';cpp.write_text(unit)
        cmd=['clang++','-std=c++17','-Wall','-Wextra','-Werror',str(cpp),'-o',str(exe)]
        if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run(cmd,check=True);subprocess.run([str(exe)],check=True)
if __name__=='__main__':main()
