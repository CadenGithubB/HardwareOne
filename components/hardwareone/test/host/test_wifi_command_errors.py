#!/usr/bin/env python3
"""Run real Wi-Fi command refusal selection against controlled service boundaries."""
from pathlib import Path
import subprocess,tempfile
from test_web_batch_handlers import extract_block
HERE=Path(__file__).resolve().parent
COMPONENT=HERE.parents[1]
def main():
    source=(COMPONENT/'System_WiFi.cpp').read_text()
    body='\n'.join(extract_block(source,m) for m in ('static const char* wifiStartFailure(', 'const char* cmd_wificonnect('))
    unit=(HERE/'wifi_command_errors_harness.cpp').read_text().replace('// INSERT_COMMANDS',body)
    with tempfile.TemporaryDirectory(prefix='hw1-wifi-command-errors-') as tmp:
        src=Path(tmp)/'checks.cpp';binary=Path(tmp)/'checks';src.write_text(unit)
        subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(src),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
    print('Wi-Fi command errors: disabled, companion refusal, startup failure, empty/busy saved list, join failure and success passed')
if __name__=='__main__':main()
