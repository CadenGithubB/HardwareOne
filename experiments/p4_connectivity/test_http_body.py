"""Offline bounded-stream checks extracted from the actual S3 HTTP fixture.

Driver/hash substitutes verify which bytes are consumed, not SHA-256 itself.
The Python HTTP runner verifies real small-response hashes during device tests.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parent / "private/app/components/hardwareone/Connectivity_HttpProbe.cpp"
COMPILER = shutil.which("c++")

STUBS = r"""
#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
constexpr size_t MAX_BODY=2000000, SMALL_BODY=8192;
uint32_t now=0;
uint32_t millis() { return now; }
bool allocationFailure=false, hashFailure=false;
enum class AllocPolicy { PreferPSRAM };
void* ps_alloc(size_t n, AllocPolicy, const char*) { return allocationFailure ? nullptr : malloc(n); }
void ps_free(void* p) { free(p); }
class Stream {
 public:
  virtual ~Stream() = default;
  virtual int available()=0;
  virtual int read()=0;
  virtual int peek()=0;
  virtual void flush()=0;
  virtual size_t write(uint8_t)=0;
  virtual size_t write(const uint8_t*,size_t)=0;
  bool error=false;
  void setWriteError() { error=true; }
};
struct mbedtls_sha256_context { std::vector<uint8_t> bytes; };
void mbedtls_sha256_init(mbedtls_sha256_context*) {}
void mbedtls_sha256_free(mbedtls_sha256_context*) {}
int mbedtls_sha256_starts(mbedtls_sha256_context*, int) { return hashFailure ? -1 : 0; }
int mbedtls_sha256_update(mbedtls_sha256_context* c,const uint8_t* p,size_t n) {
  if(hashFailure) return -1;
  c->bytes.insert(c->bytes.end(),p,p+n);
  return 0;
}
int mbedtls_sha256_finish(mbedtls_sha256_context*,uint8_t out[32]) {
  if(hashFailure) return -1;
  memset(out,0xab,32); return 0;
}
"""

CHECKS = r"""
int main() {
  {
    BodySummary body;
    const char* chunks[]={"<!doctype html><HT", "ML>user", "name pass", "word</ht", "ml>"};
    size_t length=0;
    for(auto part:chunks) {
      size_t n=strlen(part); length+=n;
      assert(body.write((const uint8_t*)part,n)==n);
    }
    assert(body.count==length && body.hash.bytes.size()==length);
    assert(memcmp(body.small,body.hash.bytes.data(),length)==0);
    assert(body.htmlOpen.found && body.htmlClose.found && body.username.found && body.password.found);
    char hex[65]; assert(body.finish(hex));
    assert(strlen(hex)==64 && hex[0]=='a' && hex[1]=='b');
  }
  {
    BodySummary body;
    std::vector<uint8_t> large(MAX_BODY,'x');
    assert(body.write(large.data(),large.size())==MAX_BODY);
    assert(body.count==MAX_BODY && body.hash.bytes==large);
    assert(!body.htmlOpen.found && !body.htmlClose.found);
    assert(body.write(uint8_t('x'))==0 && body.failed && body.error);
    assert(body.count==MAX_BODY && body.hash.bytes.size()==MAX_BODY);
    char hex[65]; assert(!body.finish(hex));
  }
  {
    BodySummary body;
    const uint8_t incomplete[]="<html>not closed";
    assert(body.write(incomplete,sizeof(incomplete)-1)==sizeof(incomplete)-1);
    assert(body.htmlOpen.found && !body.htmlClose.found);
    now+=20000;
    assert(body.write(uint8_t('x'))==0 && body.failed && body.error);
  }
  {
    BodySummary body;
    hashFailure=true;
    assert(body.write(uint8_t('x'))==0 && body.count==0 && body.failed);
    hashFailure=false;
  }
  {
    allocationFailure=true;
    BodySummary body;
    assert(body.failed && body.write(uint8_t('x'))==0);
    allocationFailure=false;
  }
}
"""


@unittest.skipUnless(COMPILER and SOURCE.is_file(), "Prepared HTTP fixture and host C++ compiler required")
class HttpBodyTests(unittest.TestCase):
    def test_actual_stream_bounds_completeness_and_cross_chunk_markers(self):
        source = SOURCE.read_text()
        extracted = source[source.index("struct Matcher {"):source.index("bool updateCookie(")]
        with tempfile.TemporaryDirectory(prefix="hw1-http-body-") as directory:
            work = Path(directory)
            cpp = work / "checks.cpp"
            cpp.write_text(STUBS + extracted + CHECKS)
            compiled = subprocess.run([COMPILER, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(work / "checks")],
                                      capture_output=True, text=True, timeout=30)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            checked = subprocess.run([str(work / "checks")], capture_output=True, text=True, timeout=10)
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)


if __name__ == "__main__":
    unittest.main()
