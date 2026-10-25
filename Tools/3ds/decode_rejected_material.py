#!/usr/bin/env python3
"""Decode the last native material rejection from a private Azahar text log."""
import argparse
import json
import re
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--log', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
text = a.log.read_text()
start = text.rfind('Native draw rejected:')
if start < 0:
    p.error('The log contains no native draw rejection')
text = text[start:]
match = re.search(r'Native material counts (\d+) (\d+) (\d+) (\d+) masks (\d+) (\d+) (\d+)', text)
if not match:
    p.error('The rejection has no decoded material state')
counts = list(map(int, match.groups()))
bp = {int(r, 16): int(v, 16) for r, v in re.findall(r'Native raster BP ([0-9a-f]+) valid=1 value=([0-9a-f]+)', text)}
registers = []
for row in re.findall(r'Native material register (\d+) color ([\d -]+) konst ([\d ]+) swap ([\d ]+)', text):
    registers.append(dict(index=int(row[0]), color=list(map(int, row[1].split())),
                          konst=list(map(int, row[2].split())), swap=list(map(int, row[3].split()))))
def operation(word):
    bias, scale, op = word >> 16 & 3, word >> 20 & 3, word >> 18 & 1
    return dict(operation=8+2*scale+op if bias == 3 else op, bias=0 if bias == 3 else bias,
                scale=0 if bias == 3 else scale, clamp=word >> 19 & 1, output=word >> 22 & 3)
stages = []
for i in range(counts[0]):
    color, alpha = bp[0xc0+2*i], bp[0xc1+2*i]
    order, konst = bp[0x28+i//2] >> (12*(i % 2)), bp[0xf6+i//2] >> (10*(i % 2))
    stages.append(dict(color=[color >> (12-4*j) & 15 for j in range(4)],
                       alpha=[alpha >> (13-3*j) & 7 for j in range(4)],
                       colorOp=operation(color), alphaOp=operation(alpha),
                       texture=order & 7, coordinate=order >> 3 & 7, textureEnabled=bool(order & 64),
                       raster=order >> 7 & 7, konstColor=konst >> 4 & 31, konstAlpha=konst >> 9 & 31,
                       rasterSwap=alpha & 3, textureSwap=alpha >> 2 & 3, indirect=bp[0x10+i]))
result = dict(origin='Decoded native rejection telemetry; no image data or hardware proof',
              log=str(a.log.resolve()), failure=text.splitlines()[0],
              stageCount=counts[0], texgenCount=counts[1], channelCount=counts[2], indirectCount=counts[3],
              colorValid=counts[4], konstValid=counts[5], swapValid=counts[6], registers=registers, stages=stages)
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text(json.dumps(result, indent=2)+'\n')
print(json.dumps(dict(failure=result['failure'], stages=len(stages), output=str(a.output))))
