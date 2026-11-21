"""Adapt the two cartridge packers to the desktop progress protocol."""
from pathlib import Path
import os
import sys
import tempfile

# The packaged interpreter runs in isolated mode.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from twilight_pack import emit, publish


def convert(port, source, output):
    if os.path.lexists(output):
        raise FileExistsError('The output already exists.')
    last = None

    def report(phase, percent):
        nonlocal last
        state = phase, min(99, percent)
        if state != last:
            emit('progress', 'cartridge', state[1], 100, phase)
            last = state

    if port == 'mk64':
        from mk64 import core
        core.convert(source, output, report)
    elif port == 'sm64ds':
        from sm64ds import core
        phases = {'Checking ROM': (0, 20), 'Creating .sm64ds': (20, 50), 'Verifying .sm64ds': (70, 29)}

        def progress(label, done, total):
            start, width = phases[label]
            report(label, start + width * done // total)

        # The original DS packer replaces the requested destination. Give it a
        # private target and publish the verified bytes with no-replace semantics.
        with tempfile.TemporaryDirectory(dir=output.parent, prefix='.forge-sm64ds-') as temp:
            staging = Path(temp) / output.name
            core.convert(source, staging, progress)
            publish(staging, output)
    else:
        raise ValueError('Unknown game.')
    size = output.stat().st_size
    emit('complete', 'complete', size, size, 'Game data verified', output=str(output), bytes=size)


if __name__ == '__main__':
    try:
        convert(sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3]))
    except Exception as error:
        emit('error', 'error', message=str(error))
        sys.exit(1)
