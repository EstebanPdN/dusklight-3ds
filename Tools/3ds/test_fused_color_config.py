#!/usr/bin/env python3
"""Host-only exact opt-in parser, one-shot SD reader and startup-call checks."""
from pathlib import Path
import argparse,hashlib,json,os,subprocess
ROOT=Path(__file__).resolve().parents[2]
N=ROOT/'Source/platforms/3ds'

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    inputs=[Path(__file__).resolve(),*[N/p for p in [
        'runtime/fused_color_config.cpp','runtime/fused_color_config.hpp',
        'tests/fused_color_config_test.cpp','runtime/game_startup.cpp',
        'gx_texture_pica.hpp','runtime/gx_state.hpp','runtime/texture_gx.hpp']]]
    sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    hashes={str(p):sha(p) for p in inputs}
    rec={'status':'running','scope':'Host ASan/UBSan; actual parser/SD reader and verbatim startup call with a recording configure mock; no game boot or GPU execution','input_sha256':hashes,'commands':[]}
    env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1:abort_on_error=1:detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'}
    def run(name,command,cwd=out):
        command=list(map(str,command));rec['commands'].append({'name':name,'argv':command,'cwd':str(cwd)})
        result=subprocess.run(command,cwd=cwd,capture_output=True,text=True,env=env,timeout=90)
        (out/(name+'.log')).write_text(result.stdout+result.stderr);result.check_returncode()
    try:
        source=(N/'runtime/game_startup.cpp').read_text()
        start=source.index('    const bool fusedColorCopy=readFusedColorCopyConfigFromSd();')
        end=source.index('\n',source.index('if(!configureGxTextureCache(',start))+1
        (out/'startup_cache.inc').write_text(source[start:end])
        run('build',['clang++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(out),
            N/'runtime/fused_color_config.cpp',N/'tests/fused_color_config_test.cpp','-o',out/'test'])
        cases=[('on',b'enabled=1\n'),('off',b'enabled=0\n'),('empty',b''),
            ('suffix',b'enabled=1\nX'),('crlf',b'enabled=1\r\n'),('nul',b'enabled=1\x00'),
            ('large',b'enabled=1\n'+bytes(4096)),('missing',None),('unreadable',False)]
        for name,token in cases:
            cwd=out/name;sd=cwd/'sdmc:/3ds/dusklight';sd.mkdir(parents=True);path=sd/'fused-color-copy.txt'
            if token is False:path.mkdir()
            elif token is not None:path.write_bytes(token)
            run(name,[out/'test','on' if name=='on' else 'off'],cwd)
        assert hashes=={str(p):sha(p) for p in inputs}
        rec.update(status='pass',inputs_unchanged=True,binary_sha256=sha(out/'test'))
    except (OSError,ValueError,AssertionError,subprocess.SubprocessError) as error:
        rec.update(status='fail',error=repr(error))
    finally:
        (out/'results.json').write_text(json.dumps(rec,indent=2)+'\n')
    print(rec['status']);return rec['status']!='pass'

if __name__=='__main__':raise SystemExit(main())
