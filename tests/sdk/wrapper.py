#!/usr/bin/env python3
"""Check wrapper phase selection and argument preservation without hardware."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='nds sdk ') as temp:
 prefix=Path(temp);(prefix/'bin').mkdir();(prefix/'share/nds-sdk').mkdir(parents=True)
 fake=prefix/'fake compiler';fake.write_text('#!/usr/bin/env python3\nimport sys,json; print(json.dumps(sys.argv[1:]))\n');fake.chmod(0o755)
 wrapper=prefix/'bin/nds-clang++';shutil.copyfile(root/'sdk/nds-clang++',wrapper);wrapper.chmod(0o755)
 (prefix/'share/nds-sdk/wrapper.json').write_text(json.dumps({'compiler':str(fake)}))
 def run(*args):return json.loads(subprocess.check_output([str(wrapper),*args],text=True))
 args=run('-fopenmp','-fopenmp-targets=arm-nds-linux-gnueabihf','-nds-platform=/some platform/file','some app.cpp','-o','some output')
 assert 'some app.cpp' in args and '-nds-platform=/some platform/file' in args
 assert args.index('-lnd')>args.index('some app.cpp')
 assert '-mcpu=cortex-a7' not in args and '-fno-addrsig' not in args
 plain=run('main.cpp');assert '-fopenmp' not in plain and not any(x.startswith('-fopenmp-targets') for x in plain)
 for phase in ['-c','-S','-E','-fsyntax-only','-M','-MM']:
  assert '-lnd' not in run(phase,'some app.cpp')
 assert run('--version')==['--version']
 assert '-lnd' not in run('-nostdlib','main.cpp')
 response=prefix/'compile flags.rsp';response.write_text('-c "some app.cpp"')
 assert '-lnd' not in run('@'+str(response))
 shown=subprocess.check_output([str(wrapper),'--show','main.cpp'],text=True)
 assert str(fake) in shown and '-lnd' in shown
 print('PASS: wrapper preserves user arguments; no implicit offload/CPU; compile-only and response-file phases omit link libraries')
