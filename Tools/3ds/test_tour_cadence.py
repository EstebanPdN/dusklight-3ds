#!/usr/bin/env python3
"""Check binary interchange, every-picture accounting, the strict gate and truncation."""
import subprocess
import tempfile
from pathlib import Path
from contextlib import redirect_stdout, redirect_stderr
from io import StringIO

from test_story_benchmark import PLAN, RECEIPT, write_csv
from analyze_cadence import CLOCK, HEADER, ROW, analyze, metrics, read_step

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / 'Source/platforms/3ds'


def main():
    with tempfile.TemporaryDirectory() as temp:
        folder = Path(temp)
        exe = folder / 'writer'
        subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I', str(NATIVE / 'runtime'),
                        str(NATIVE / 'tests/tour_cadence_test.cpp'), str(NATIVE / 'runtime/tour_cadence.cpp'),
                        '-o', str(exe)], check=True)
        subprocess.run([str(exe), temp], check=True)
        a, b = (folder / f'cadence-{i:03}.bin' for i in (1, 2))
        rows = list(read_step(a, 1)) + list(read_step(b, 2))
        assert [r[1] for r in rows] == list(range(1, 1027))
        assert rows[-1][:3] == (2 * CLOCK, 1026, 123)
        assert (rows[-1].phase, rows[-1].flags, rows[-1].scene) == (255, 1, 3)
        assert metrics(rows)[1] > 25 and metrics(rows)[2] == 0.5 and metrics(rows)[5] == 1
        assert metrics([(CLOCK // 20,), (CLOCK // 20 + 1,)])[-1] == 1
        (folder / 'tour.txt').write_text('mode=story\nstory_version=1\nfirst_step=1\nlast_step=2\nzones=2\nresult=complete\nzones_finished=2\nfailures=0\n')
        (folder / 'summary.csv').write_text('zone,status\n1,pass\n2,pass\n')
        plan = [PLAN | {'zone': n, 'step': f'gate-{n}'} for n in (1, 2)]
        receipts = [RECEIPT | {'zone': n, 'step': f'gate-{n}'} for n in (1, 2)]
        write_csv(folder / 'story-plan.csv', plan[0], plan)
        write_csv(folder / 'story.csv', receipts[0], receipts)
        with redirect_stdout(StringIO()), redirect_stderr(StringIO()):
            assert not analyze(folder)
            data = b.read_bytes()
            good = list(rows[-1]); good[0] = CLOCK // 26
            b.write_bytes(data[:HEADER.size] + ROW.pack(*good))
            assert analyze(folder)
            # Test everything records story receipts followed by room probes.
            info = (folder / 'tour.txt').read_text()
            summary = (folder / 'summary.csv').read_text()
            (folder / 'tour.txt').write_text(info.replace('mode=story', 'mode=everything').replace('last_step=2', 'last_step=1'))
            (folder / 'summary.csv').write_text(summary.replace('2,pass', '2,ok'))
            write_csv(folder / 'story-plan.csv', plan[0], plan[:1])
            write_csv(folder / 'story.csv', receipts[0], receipts[:1])
            assert analyze(folder)
            (folder / 'summary.csv').write_text(summary)
            assert not analyze(folder)
            (folder / 'tour.txt').write_text(info)
            write_csv(folder / 'story-plan.csv', plan[0], plan)
            # A reported pass with no visible arrival does not establish coverage.
            write_csv(folder / 'story.csv', receipts[0], [receipts[0], receipts[1] | {'arrival_pictures': 0}])
            assert not analyze(folder)
            write_csv(folder / 'story.csv', receipts[0], receipts)
            good[1] += 1
            b.write_bytes(data[:HEADER.size] + ROW.pack(*good))
            assert not analyze(folder)
            b.write_bytes(data[:-1])
            try:
                list(read_step(b, 2))
            except ValueError:
                pass
            else:
                raise AssertionError('accepted partial trace')
            output, errors = StringIO(), StringIO()
            with redirect_stdout(output), redirect_stderr(errors):
                assert not analyze(folder)
            assert 'Step 1' in output.getvalue() and 'incomplete trace' in errors.getvalue()
            a.write_bytes(b'')
            b.write_bytes(data)
            output, errors = StringIO(), StringIO()
            with redirect_stdout(output), redirect_stderr(errors):
                assert not analyze(folder)
            assert 'Step 2' in output.getvalue() and 'incomplete header' in errors.getvalue()
    print('Tour cadence writer, reader and strict timing gate passed')


if __name__ == '__main__':
    main()
