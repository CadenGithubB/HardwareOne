#!/usr/bin/env python3
"""Exercise the production voice routes/loaders against MultiNet list semantics."""
import argparse
from pathlib import Path
import subprocess
import tempfile
from test_espsr_runtime import function

HERE = Path(__file__).resolve().parent

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    source = (HERE.parents[1] / 'System_ESPSR.cpp').read_text()
    text = (HERE/'espsr_grammar_harness.cpp').read_text()
    mapping = source[source.index('#define MAX_VOICE_CLI_MAPPINGS'):source.index('// Forward declarations for MultiNet helpers')]
    routes = source[source.index('struct VoiceRoute {'):source.index('// Liveness filter:')]
    text = text.replace('// INSERT_TABLES', mapping + routes)
    signatures = ['static bool routeAlive(', 'static esp_mn_phrase_t* findLoadedVoicePhrase(',
                  'static bool copyRecognizedVoicePhrase(', 'static bool loadedTargetMatches(',
                  'static bool addSpecialPhrases()', 'static bool loadTargetsForCategory(',
                  'static bool loadCategories()', 'static bool loadSubCategoriesForCategory(',
                  'static bool loadTargetsForCategorySubCategory(']
    text = text.replace('// INSERT_FUNCTIONS', '\n'.join(function(source, signature) for signature in signatures))
    with tempfile.TemporaryDirectory(prefix='hw1-sr-grammar-') as tmp:
        unit = Path(tmp)/'test.cpp'; binary = Path(tmp)/'test'
        unit.write_text(text)
        cmd = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-but-set-variable', str(unit), '-o', str(binary)]
        if args.sanitize: cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(cmd, check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == '__main__': main()
