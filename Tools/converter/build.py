"""Build portable desktop packages from macOS with Node, npm ci and Python 3.12+."""
import argparse
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import tarfile
import tempfile

SOURCE = Path(__file__).resolve().parent
ROOT = SOURCE.parents[1]
OUT = ROOT / 'Build/forge-converter'
MK_PROJECT = ROOT.parent / 'MARIO KART 64'
MK = MK_PROJECT / 'Source/platform/desktop/extractor'
DS = ROOT.parent / 'SM64DS/tools/sm64ds_packer'
PYTHON = '3.13.18+20261110'
TARGETS = {
    'mac-arm64': ('darwin', 'arm64', 'aarch64-apple-darwin', '7a2384641584ab9984221fcb0fc9265df465922af71be71410529e2a9c3f0540'),
    'mac-intel': ('darwin', 'x64', 'x86_64-apple-darwin', 'd27d59a722db9f69926eb6d9564071247221f45a04da920ea7f3001c97e32f9b'),
    'windows-x64': ('win32', 'x64', 'x86_64-pc-windows-msvc', '2e0fed09d48989f64c9305da9ff2d37a41c0d13ab2aa6283405b2b620dfbf805'),
    'linux-x64': ('linux', 'x64', 'x86_64-unknown-linux-gnu', 'f2d2199eb25880e07863b391cf7124e7ffa1338b0d65600dfcec117c4223a7fb'),
}
NAME = 'Forge 3DS Converter'


def run(*args):
    subprocess.run(args, check=True)


def icon(folder, platform):
    art = SOURCE / 'ui/art/forge.png'
    if platform == 'darwin':
        icons = folder / 'dusk.iconset'
        icons.mkdir()
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                suffix = '@2x' if scale == 2 else ''
                run('sips', '-Z', str(size * scale), art, '--out', icons / f'icon_{size}x{size}{suffix}.png')
        run('iconutil', '-c', 'icns', icons)
        return [f'--icon={folder}/dusk.icns']
    if platform == 'win32':
        data = (SOURCE / 'ui/icon.png').read_bytes()
        (folder / 'dusk.ico').write_bytes(struct.pack('<HHHBBBBHHII', 0,1,1,0,0,0,0,1,32,len(data),22) + data)
        return [f'--icon={folder}/dusk.ico']
    return []


def prepare_cartridges(backend, target):
    for name in ('cartridge.py', 'worms_pack.py'):
        shutil.copy2(SOURCE / name, backend)
    for folder, project in [('mk64', MK), ('sm64ds', DS)]:
        module = backend / folder
        module.mkdir()
        for name in ('core.py', 'CREDITS.txt'):
            shutil.copy2(project / name, module)
    shutil.copy2(DS / 'recipes.json.zlib', backend / 'sm64ds')
    shutil.copytree(MK / 'licenses', backend / 'mk64/licenses')
    runtime = backend / 'mk64/runtime'
    shutil.copytree(MK / 'runtime/metadata', runtime / 'metadata')
    bindir = runtime / 'bin'
    bindir.mkdir()
    engine = bindir / ('mk64-extract.exe' if target == 'windows-x64' else 'mk64-extract')
    if target == 'mac-arm64':
        shutil.copy2(MK / 'runtime/bin/mk64-extract', engine)
    elif target == 'mac-intel':
        builddir = OUT / 'intel-engine'
        if not (builddir / 'CMakeCache.txt').exists():
            run('cmake', '-S', MK / 'native', '-B', builddir, '-DCMAKE_BUILD_TYPE=Release',
                '-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0', '-DCMAKE_OSX_ARCHITECTURES=x86_64',
                '-DCMAKE_POLICY_VERSION_MINIMUM=3.5')
        run('cmake', '--build', builddir, '--target', 'mk64-extract', '--parallel', '4')
        shutil.copy2(builddir / 'mk64-extract', engine)
    elif target == 'windows-x64':
        builddir = OUT / 'windows-engine'
        if not (builddir / 'CMakeCache.txt').exists():
            run('cmake', '-S', SOURCE / 'windows-engine', '-B', builddir,
                f'-DMK64_EXTRACTOR={MK}', '-DCMAKE_SYSTEM_NAME=Windows',
                '-DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang',
                '-DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++',
                '-DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres',
                '-DCMAKE_DISABLE_FIND_PACKAGE_spdlog=ON', '-DCMAKE_BUILD_TYPE=Release',
                '-DCMAKE_POLICY_VERSION_MINIMUM=3.5')
        run('cmake', '--build', builddir, '--target', 'mk64-extract', '--parallel', '4')
        shutil.copy2(builddir / 'engine/mk64-extract.exe', engine)
    else:
        release = MK_PROJECT / 'Releases/v2.1-E2/assets/MK64-Extractor-Linux-x64.tar.gz'
        with tarfile.open(release) as archive:
            engine.write_bytes(archive.extractfile('MK64-Extractor-Linux-x64/app/runtime/bin/mk64-extract').read())
    engine.chmod(0o755)


def build(target):
    platform, arch, triple, sha = TARGETS[target]
    cache = ROOT / 'Build/convertidor/cache'
    OUT.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / f'cpython-{PYTHON}-{triple}-install_only_stripped.tar.gz'
    if not archive.exists():
        url = f'https://github.com/astral-sh/python-build-standalone/releases/download/20261110/{archive.name}'
        run('curl', '-fsSL', '--retry', '3', url, '-o', archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != sha:
        raise ValueError(f'Invalid checksum: {archive}')
    with tempfile.TemporaryDirectory(dir=OUT) as temp:
        work = Path(temp)
        backend = work / 'backend'
        backend.mkdir()
        with tarfile.open(archive) as stream:
            # terminfo uses case-distinct aliases that collide on common Mac volumes.
            members = (entry for entry in stream if '/share/terminfo/' not in entry.name)
            stream.extractall(backend, members=members, filter='data')
        shutil.copy2(ROOT / 'Tools/3ds/twilight_pack.py', backend)
        prepare_cartridges(backend, target)
        ui = work / 'app'
        shutil.copytree(SOURCE / 'ui', ui / 'ui')
        for entry in ('package.json', 'main.cjs', 'preload.cjs', 'ports.json'):
            shutil.copy2(SOURCE / entry, ui)
        run(SOURCE / 'node_modules/.bin/electron-packager', ui, NAME, '--electron-version=44.9.3',
            f'--platform={platform}', f'--arch={arch}', f'--out={work}', '--no-prune',
            '--app-bundle-id=org.estebansforge.romconverter', *icon(work, platform))
        product = work / f'{NAME}-{platform}-{arch}'
        if platform == 'darwin':
            product = product / f'{NAME}.app'
            shutil.copytree(backend, product / 'Contents/Resources/backend', symlinks=True)
            run('codesign', '--force', '--deep', '--sign', '-', product)
            run('codesign', '--verify', '--deep', '--strict', product)
        else:
            shutil.copytree(backend, product / 'resources/backend', symlinks=True)
        extension = '.tar.gz' if platform == 'linux' else '.zip'
        package = work / ('Forge-3DS-Converter-' + target + extension)
        if platform == 'linux':
            subprocess.run(['tar','czf',package,product.name], cwd=product.parent, check=True)
        else:
            run('ditto', '-c', '-k', '--keepParent', '--norsrc', product, package)
        package.replace(OUT / package.name)
        if target == 'mac-arm64':
            dest = OUT / product.name
            if dest.exists():
                shutil.rmtree(dest)
            shutil.move(product, dest)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target', nargs='*', help=' '.join(TARGETS))
    args = parser.parse_args()
    for target in args.target or TARGETS:
        if target not in TARGETS:
            parser.error(f'Unknown target: {target}')
        build(target)
