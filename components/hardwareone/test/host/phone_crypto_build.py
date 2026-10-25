"""Disposable native builds of the repository's pinned crypto dependencies."""
from contextlib import contextmanager
import os
from pathlib import Path
import subprocess
import tempfile
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[3]

def run(command, cwd=None):
    p=subprocess.run(list(map(str,command)),cwd=cwd,text=True,capture_output=True)
    if p.returncode:raise RuntimeError(p.stdout+p.stderr)

@contextmanager
def crypto_flags(idf=None, build=None):
    idf=Path(idf or os.environ.get('IDF_PATH', ROOT.parent/'esp-idf')).resolve()
    sodium=ROOT/'managed_components/espressif__libsodium/libsodium'
    mbed=idf/'components/mbedtls/mbedtls'
    with tempfile.TemporaryDirectory(prefix='hw1-pipe-crypto-') as tmp:
        d=Path(build) if build else Path(tmp)
        if not build:
            (d/'sodium').mkdir()
            run(['sh',sodium/'configure','--disable-shared','--enable-static','--disable-asm','--disable-dependency-tracking'],d/'sodium')
            run(['make','-j4','MKDIR_P=mkdir -p'],d/'sodium')
            run(['cmake','-S',mbed,'-B',d/'mbedtls','-DENABLE_PROGRAMS=OFF','-DENABLE_TESTING=OFF','-DUSE_SHARED_MBEDTLS_LIBRARY=OFF'])
            run(['cmake','--build',d/'mbedtls','-j4'])
        yield (['-I',str(d/'sodium/src/libsodium/include'),'-I',str(sodium/'src/libsodium/include'),
                '-I',str(sodium/'src/libsodium/include/sodium'),'-I',str(mbed/'include')],
               [str(d/'sodium/src/libsodium/.libs/libsodium.a'),str(d/'mbedtls/library/libmbedcrypto.a')])
