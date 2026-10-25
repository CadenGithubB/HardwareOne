#!/usr/bin/env python3
"""Certificate and sealed-byte tests with the device's ESP-IDF mbedTLS config.

Supply the IDF tree and a generated build/config/sdkconfig.h. The native library
uses the actual esp_config.h, replacing hardware, allocator, entropy, time and
mutex port hooks only. Protocol, certificate/date and ticket choices stay intact.
Synthetic keys and the temporary native build are removed when the test ends.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
from test_web_batch_handlers import extract_block
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
def run(args,cwd=None):
    result=subprocess.run(list(map(str,args)),cwd=cwd,text=True,capture_output=True)
    if result.returncode:raise RuntimeError('Command failed: '+str(args[0])+'\n'+result.stdout+result.stderr)
    return result.stdout
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--idf',type=Path,required=True)
    p.add_argument('--sdkconfig',type=Path,required=True)
    p.add_argument('--cxx',default=os.environ.get('CXX','clang++'))
    p.add_argument('--sanitize',action='store_true');args=p.parse_args()
    sdkconfig=args.sdkconfig.resolve(); sdktext=sdkconfig.read_text()
    date_checks=bool(re.search(r'^#define CONFIG_MBEDTLS_HAVE_TIME_DATE\b',sdktext,re.M))
    source=(ROOT/'System_LLMHttpTransport.cpp').read_text()
    pieces='\n'.join(extract_block(source,k)+';' for k in ('struct PhoneTls','struct LlmHttpIo'))
    functions='\n'.join(extract_block(source,k) for k in ('bool cancelled(', 'bool expired(', 'uint32_t budget(',
        'void closeChild(', 'int bioSend(', 'int bioReceive(', 'bool retryTls(', 'bool configurePhoneTls(',
        'Failure phoneOpenFailure(', 'bool connectPhone(', 'int read(', 'int write('))
    unit=(HERE/'llm_phone_tls_harness.cpp').read_text().replace('// INSERT_TYPES',pieces).replace('// INSERT_PRODUCTION',functions).replace('// INSERT_LIMITS', '[[maybe_unused]] ' + re.search(r'constexpr uint32_t kDirectMs=.*?;', source).group())
    with tempfile.TemporaryDirectory(prefix='hw1-phone-tls-') as tmp:
        d=Path(tmp);cpp=d/'client.cpp';binary=d/'client';cpp.write_text(unit)
        native_config=d/'device_mbedtls_config.h'
        hardware=re.findall(r'^#define (CONFIG_MBEDTLS_HARDWARE_\w+)\b',sdktext,re.M)
        native_config.write_text('#pragma once\n#include "'+str(sdkconfig)+'"\n'+
            '\n'.join('#undef '+name for name in hardware)+'''\n
#undef CONFIG_MBEDTLS_ROM_MD5
#define CONFIG_IDF_TARGET_LINUX 1
#define CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC 1
'''+ '#include "'+str(args.idf.resolve()/'components/mbedtls/port/include/mbedtls/esp_config.h')+'"\n'+'''
#undef MBEDTLS_ENTROPY_HARDWARE_ALT
#undef MBEDTLS_NO_PLATFORM_ENTROPY
#undef MBEDTLS_PLATFORM_MS_TIME_ALT
#ifdef MBEDTLS_THREADING_ALT
#undef MBEDTLS_THREADING_ALT
#define MBEDTLS_THREADING_PTHREAD
#endif
''')
        mbedtls_source=args.idf.resolve()/'components/mbedtls/mbedtls'
        mbedtls_build=d/'mbedtls-build'
        flags='-I'+str(sdkconfig.parent)
        if args.sanitize:flags+=' -fsanitize=address,undefined -fno-omit-frame-pointer'
        run(['cmake','-S',mbedtls_source,'-B',mbedtls_build,'-DENABLE_TESTING=OFF',
             '-DENABLE_PROGRAMS=OFF','-DMBEDTLS_FATAL_WARNINGS=OFF',
             '-DMBEDTLS_CONFIG_FILE='+str(native_config),'-DCMAKE_C_FLAGS='+flags])
        run(['cmake','--build',mbedtls_build,'-j','4'])
        command=[args.cxx,'-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-parameter',
                 '-DMBEDTLS_CONFIG_FILE="'+str(native_config)+'"',
                 '-I',ROOT,'-I',mbedtls_source/'include','-I',sdkconfig.parent,cpp,
                 mbedtls_build/'library/libmbedtls.a',mbedtls_build/'library/libmbedx509.a',
                 mbedtls_build/'library/libmbedcrypto.a','-o',binary]
        if args.sanitize:command[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']
        run(command)
        run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout','ca.key','-out','ca.pem',
             '-days','3650','-subj','/CN=HardwareOne test CA'],d)
        run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout','other.key','-out','other.pem',
             '-days','3650','-subj','/CN=Untrusted test CA'],d)
        run(['openssl','req','-new','-newkey','rsa:2048','-nodes','-keyout','leaf.key','-out','leaf.csr',
             '-subj','/CN=relay.example'],d)
        (d/'index').write_text('');(d/'serial').write_text('01\n');(d/'issued').mkdir()
        cases=[
          ('dns-valid','relay.example','DNS:relay.example','ca.pem',None,True),
          ('dns-wrong','relay.example','DNS:other.example','ca.pem',None,False),
          ('ip-valid','192.0.2.10','IP:192.0.2.10','ca.pem',None,True),
          ('ip-wrong','192.0.2.10','IP:192.0.2.11','ca.pem',None,False),
          ('ipv6-valid','2001:db8::1','IP:2001:db8::1','ca.pem',None,True),
          ('ipv6-wrong','2001:db8::1','IP:2001:db8::2','ca.pem',None,False),
          ('ca-wrong','relay.example','DNS:relay.example','other.pem',None,False),
          ('expired','relay.example','DNS:relay.example','ca.pem',('20200101000000Z','20210101000000Z'),not date_checks),
          ('future','relay.example','DNS:relay.example','ca.pem',('20400101000000Z','20410101000000Z'),not date_checks),
        ]
        for name,hostname,san,ca,dates,expected in cases:
            config=f'''[ca]
default_ca=issuer
[issuer]
database=index
serial=serial
new_certs_dir=issued
certificate=ca.pem
private_key=ca.key
default_md=sha256
default_days=365
policy=policy
unique_subject=no
x509_extensions=leaf
[policy]
commonName=supplied
[leaf]
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName={san}
'''
            (d/'ca.cnf').write_text(config)
            command=['openssl','ca','-batch','-config','ca.cnf','-in','leaf.csr','-out','leaf.pem','-notext']
            if dates:command+=['-startdate',dates[0],'-enddate',dates[1]]
            run(command,d)
            for fragment in (8,500):
                output=run([binary,d/ca,d/'leaf.pem',d/'leaf.key',hostname,int(expected),fragment])
                print(name,'fragment',fragment,output.strip())
