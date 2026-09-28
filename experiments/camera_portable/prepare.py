#!/usr/bin/env python3
"""Build-only portable-camera overlay on the qualified battery application copies."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import yaml

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
BASE = '8d9562e'
COMPONENT = 'components/hardwareone/'
DEP_FILE = COMPONENT + 'idf_component.yml'
PROBE = HERE.parent / 'board_qualification/camera_probe/p4'
PACKAGES = ('cmake_utilities', 'esp_cam_sensor', 'esp_h264', 'esp_ipa', 'esp_sccb_intf', 'esp_video', 'usb_host_uvc')

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def battery_module():
    spec=importlib.util.spec_from_file_location('camera_battery_baseline', HERE.parent/'battery_portable/prepare.py')
    mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod);return mod

def capture():
    names=subprocess.check_output(['git','diff','--name-only',BASE,'--',COMPONENT],cwd=REPO).decode().splitlines()
    names+=subprocess.check_output(['git','ls-files','--others','--exclude-standard','--',COMPONENT],cwd=REPO).decode().splitlines()
    names=sorted(set(p for p in names if '/test/' not in p and Path(p).suffix in ('.h','.cpp','.txt')))
    tracked=set(subprocess.check_output(['git','ls-files','--',COMPONENT],cwd=REPO).decode().splitlines())
    chunks=[]
    for path in names:
        if path in tracked: chunks.append(subprocess.check_output(['git','diff','--unified=2','--binary',BASE,'--',path],cwd=REPO))
        else:
            result=subprocess.run(['git','diff','--no-index','--binary','--','/dev/null',path],cwd=REPO,capture_output=True)
            if result.returncode!=1: raise ValueError('New file patch failed: '+path)
            chunks.append(result.stdout)
    (HERE/'integration.patch').write_bytes(b''.join(chunks))
    record={'schema':1,'base_commit':BASE,'production_sources':{p:sha(REPO/p) for p in names},
            'dependency_source_sha256':sha(REPO/DEP_FILE),'patch_sha256':sha(HERE/'integration.patch')}
    (HERE/'integration-inputs.json').write_text(json.dumps(record,indent=2)+'\n')
    print('Captured',len(names),'production sources')

def inputs(target):
    data=json.loads((HERE/'integration-inputs.json').read_text())
    assert data['patch_sha256']==sha(HERE/'integration.patch')
    assert data['dependency_source_sha256']==sha(REPO/DEP_FILE)
    for p,d in data['production_sources'].items():
        if sha(REPO/p)!=d:raise ValueError('Source changed; capture and refresh: '+p)
    lock=HERE/('dependencies-'+target+'.lock')
    return {'integration_inputs_sha256':sha(HERE/'integration-inputs.json'),
            'prepare_sha256':sha(HERE/'prepare.py'),
            'jpeg_probe_sha256':sha(HERE/'jpeg_probe.h'),
            'profile_sha256':sha(HERE/('features-'+target+'.h')),
            'sdkconfig_sha256':sha(HERE/('sdkconfig.'+target+'.camera.defaults')),
            'radio_shims':{p:sha(HERE/p) for p in ('radio_backend.cpp','radio_backend.h')},
            'camera_probe_lock_sha256':sha(PROBE/'dependencies.lock'),
            'qualified_lock_sha256':sha(lock) if lock.exists() else None}

def baseline(target):
    battery=battery_module();source=HERE.parent/('battery_portable/private/app-'+target)
    battery.check(target,source,current=False)
    _,expected,common=battery.baseline(target)
    expected.update(json.loads((source/'battery-build-manifest.json').read_text())['overlay'])
    return source,expected,common

def lock_name(target): return 'dependencies.lock.esp32p4' if target == 'p4' else 'dependencies.lock'

def expected_lock(target):
    source,_,_=baseline(target)
    result=yaml.safe_load((source/lock_name(target)).read_text())
    if target=='p4':
        extra=yaml.safe_load((PROBE/'dependencies.lock').read_text())
        for key,entry in extra['dependencies'].items():
            if key in result['dependencies'] and result['dependencies'][key]!=entry:
                # SDK entry formatting can differ, but the pinned version cannot.
                assert key=='idf' and result['dependencies'][key]['version']==entry['version']
            else: result['dependencies'][key]=entry
        result['direct_dependencies']=sorted(set(result['direct_dependencies']+['espressif/esp_video']))
    return result

def verify_lock(target,path):
    wanted=expected_lock(target);actual=yaml.safe_load(path.read_text())
    assert actual['target']==wanted['target']
    assert actual['dependencies']==wanted['dependencies'],'Dependency set/version/hash changed'
    assert set(actual['direct_dependencies'])==set(wanted['direct_dependencies'])

def check(target,dest,current=True):
    source,expected,common=baseline(target)
    data=json.loads((dest/'camera-build-manifest.json').read_text())
    assert data['schema']==1 and data['target']==target
    assert data['baseline_manifest_sha256']==sha(source/'battery-build-manifest.json')
    if current and data['inputs']!=inputs(target):raise ValueError('Build inputs changed')
    expected.update(data['overlay']);common.verify_files(dest,expected,'camera application')
    return len(expected)


def apply_jpeg_probe(stage):
    """Inject the private serial-admin probe without editing production files."""
    name=COMPONENT+'Experiment_CameraJpegProbe.h'
    shutil.copy2(HERE/'jpeg_probe.h',stage/name)
    filesystem=COMPONENT+'System_Filesystem.cpp'
    path=stage/filesystem;source=path.read_text()
    include='#include "System_Filesystem_Internal.h"'
    command='  { "fileread",'
    if source.count(include)!=1 or source.count(command)!=1 or 'cmd_camerajpegprobe' in source:
        raise ValueError('JPEG probe insertion points changed')
    source=source.replace(include,include+'\n#include "Experiment_CameraJpegProbe.h"',1)
    entry='  { "camerajpegprobe", "Experiment only: validate a stored JPEG using the software decoder.", true, cmd_camerajpegprobe },\n'
    source=source.replace(command,entry+command,1)
    path.write_text(source)
    return {name,filesystem}

def prepare(target,dest,refresh=False):
    if dest.exists() and not refresh:raise ValueError('Destination exists; use --check or --refresh')
    if refresh:check(target,dest,current=False)
    source,expected,common=baseline(target);inp=inputs(target)
    changes=json.loads((HERE/'integration-inputs.json').read_text())['production_sources']
    dest.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.camera-stage-',dir=dest.parent) as directory:
        stage=Path(directory)
        for rel in expected:
            out=common.safe_path(stage,rel);out.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(common.safe_path(source,rel),out)
        result=subprocess.run(['patch','-f','-F','0','-p','1'],input=(HERE/'integration.patch').read_bytes(),cwd=stage,capture_output=True)
        output=(result.stdout+result.stderr).decode()
        if result.returncode or 'fuzz' in output:raise ValueError('Exact-context patch failed: '+output)
        if target=='p4':
            dependency=stage/DEP_FILE
            assert 'espressif/esp_video:' not in dependency.read_text()
            dependency.write_text(dependency.read_text()+'\n  espressif/esp_video:\n    version: "==2.2.0"\n')
        lock=HERE/('dependencies-'+target+'.lock')
        if lock.exists():verify_lock(target,lock);shutil.copy2(lock,stage/lock_name(target))
        else:(stage/lock_name(target)).write_text(yaml.safe_dump(expected_lock(target),sort_keys=False))
        probe_overlay=apply_jpeg_probe(stage)
        overlay=set(changes)|{DEP_FILE,lock_name(target)}|probe_overlay
        data={'schema':1,'target':target,'baseline_manifest_sha256':sha(source/'battery-build-manifest.json'),
              'inputs':inp,'overlay':{p:sha(stage/p) for p in sorted(overlay)}}
        (stage/'camera-build-manifest.json').write_text(json.dumps(data,indent=2)+'\n');count=check(target,stage)
        if refresh:
            old=json.loads((dest/'camera-build-manifest.json').read_text())
            for rel in set(old['overlay'])|set(data['overlay']):
                src,out=stage/rel,dest/rel
                if not src.is_file():raise ValueError('Removing an overlay needs a new copy')
                out.parent.mkdir(parents=True,exist_ok=True)
                if not out.exists() or sha(src)!=sha(out):shutil.copy2(src,out)
            shutil.copy2(stage/'camera-build-manifest.json',dest/'camera-build-manifest.json')
        else:
            shutil.copytree(stage,dest)
            shutil.copytree(source/'managed_components',dest/'managed_components')
            if target=='p4':
                cache=HERE.parent/'board_qualification/private/camera-repro-p4/managed_components'
                for package in PACKAGES:
                    name='espressif__'+package
                    assert not (dest/'managed_components'/name).exists()
                    shutil.copytree(cache/name,dest/'managed_components'/name)
    return count

def accept_lock(target,dest):
    # Component Manager can update its aggregate manifest hash on first resolve.
    # No version/hash/dependency drift is accepted by this operation.
    verify_lock(target,dest/lock_name(target))
    shutil.copy2(dest/lock_name(target),HERE/('dependencies-'+target+'.lock'))
    data=json.loads((dest/'camera-build-manifest.json').read_text())
    data['overlay'][lock_name(target)]=sha(dest/lock_name(target));data['inputs']=inputs(target)
    (dest/'camera-build-manifest.json').write_text(json.dumps(data,indent=2)+'\n')
    return check(target,dest)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--capture',action='store_true');p.add_argument('--target',choices=('p4','s3'));p.add_argument('--check',action='store_true');p.add_argument('--refresh',action='store_true');p.add_argument('--accept-lock',action='store_true');args=p.parse_args()
    if args.capture:return capture()
    if not args.target:p.error('--target required')
    dest=HERE/('private/app-'+args.target)
    count=accept_lock(args.target,dest) if args.accept_lock else check(args.target,dest) if args.check else prepare(args.target,dest,args.refresh)
    print('Verified',count,'sources:',dest)
if __name__=='__main__':main()
