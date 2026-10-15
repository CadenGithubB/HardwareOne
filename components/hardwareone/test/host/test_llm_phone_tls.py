#!/usr/bin/env python3
"""Certificate and sealed-byte tests using production BIO/setup and real SDK mbedTLS.

Build the pinned SDK's mbedtls directory out of tree with ENABLE_TESTING=OFF and
ENABLE_PROGRAMS=OFF, then supply its source and build paths. Temporary CA/leaf
keys are synthetic and removed when the test finishes. No relay/Android needed.
"""
import argparse
from pathlib import Path
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
    p=argparse.ArgumentParser();p.add_argument('--mbedtls-source',type=Path,required=True);p.add_argument('--mbedtls-build',type=Path,required=True);p.add_argument('--sanitize',action='store_true');args=p.parse_args()
    source=(ROOT/'System_LLMHttpTransport.cpp').read_text()
    pieces='\n'.join(extract_block(source,k)+';' for k in ('struct PhoneTls','struct LlmHttpIo'))
    functions='\n'.join(extract_block(source,k) for k in ('bool cancelled(', 'bool expired(', 'uint32_t budget(',
        'void closeChild(', 'int bioSend(', 'int bioReceive(', 'bool retryTls(', 'bool configurePhoneTls(',
        'bool connectPhone(', 'int read(', 'int write('))
    unit=(HERE/'llm_phone_tls_harness.cpp').read_text().replace('// INSERT_TYPES',pieces).replace('// INSERT_PRODUCTION',functions)
    with tempfile.TemporaryDirectory(prefix='hw1-phone-tls-') as tmp:
        d=Path(tmp);cpp=d/'client.cpp';binary=d/'client';cpp.write_text(unit)
        command=['clang++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-parameter',
                 '-I',ROOT,'-I',args.mbedtls_source/'include',cpp,
                 args.mbedtls_build/'library/libmbedtls.a',args.mbedtls_build/'library/libmbedx509.a',
                 args.mbedtls_build/'library/libmbedcrypto.a','-o',binary]
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
          ('expired','relay.example','DNS:relay.example','ca.pem',('20200101000000Z','20210101000000Z'),False),
          ('future','relay.example','DNS:relay.example','ca.pem',('20400101000000Z','20410101000000Z'),False),
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
