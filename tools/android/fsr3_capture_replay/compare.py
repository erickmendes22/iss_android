#!/usr/bin/env python3
"""Compare a captured baseline and a replay; optical flow is a reference, not ground truth."""
import argparse
import json
from pathlib import Path
import cv2
import numpy as np


def image(path):
    return np.fromfile(path, np.uint8).reshape(720, 1280, 4)[:, :, :3].astype(np.float32)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('capture', type=Path)
    p.add_argument('baseline', type=Path)
    p.add_argument('candidate', type=Path)
    p.add_argument('--motion', type=Path, required=True)
    p.add_argument('--roi', required=True, help='x,y,width,height in input pixels; verify for this capture')
    args = p.parse_args()
    x, y, w, h = [int(v) for v in args.roi.split(',')]
    if x < 0 or y < 0 or w <= 0 or h <= 0 or x+w > 640 or y+h > 360:
        p.error('ROI is outside the 640x360 input')
    mask = np.zeros((360, 640), bool)
    mask[y:y+h, x:x+w] = True
    inputs = [np.fromfile(args.capture/f'{f}_input_640x360_rgba.raw', np.uint8).reshape(360,640,4)[:, :, :3]
              for f in ('f000', 'f001')]
    gray = [cv2.cvtColor(a, cv2.COLOR_RGB2GRAY) for a in inputs]
    flow = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM).calc(gray[1], gray[0], None)
    jitter = []
    for frame in ('f000', 'f001'):
        info = dict(line.split('=', 1) for line in (args.capture/f'{frame}_fsr_info.txt').read_text().splitlines() if '=' in line)
        jitter.append(np.array([float(v) for v in info['sdk_jitter'].split(',')]))
    flow += jitter[1] - jitter[0]
    old = np.fromfile(args.capture/'f001_motion_640x360_rg16f.raw', np.float16).reshape(360,640,2).astype(np.float32)
    new = np.fromfile(args.motion/'f001_motion_640x360_rg16f.raw', np.float16).reshape(360,640,2).astype(np.float32)
    changed = np.max(np.abs(new-old), axis=2) > .001
    chosen = changed & mask
    yy, xx = np.mgrid[0:360, 0:640]
    mx = ((xx+.5-jitter[1][0])*2-.5).astype(np.float32)
    my = ((yy+.5-jitter[1][1])*2-.5).astype(np.float32)
    previous = image(args.capture/'f000_output_1280x720_rgba.raw')
    reference = cv2.remap(previous, mx+2*flow[:, :, 0], my+2*flow[:, :, 1], cv2.INTER_LINEAR)
    phone = image(args.capture/'f001_output_1280x720_rgba.raw')
    result = {'roi': [x,y,w,h], 'reference': 'DIS backward flow with captured jitter removed; not ground truth',
              'changed_viewport_pixels': int(changed.sum()), 'changed_roi_pixels': int(chosen.sum())}
    if chosen.any():
        result['motion_reference_error_pixels'] = {
            'baseline': float(np.linalg.norm(old[chosen]-flow[chosen], axis=1).mean()),
            'candidate': float(np.linalg.norm(new[chosen]-flow[chosen], axis=1).mean())}
    for name, directory in [('baseline',args.baseline), ('candidate',args.candidate)]:
        output = image(directory/'f001_output_1280x720_rgba.raw')
        sampled = cv2.remap(output, mx, my, cv2.INTER_LINEAR)
        result[name] = {'phone_rgb_mae_8bit': float(np.abs(output-phone).mean()),
                        'roi_current_sample_mae_8bit': float(np.abs(sampled[mask]-inputs[1][mask]).mean()),
                        'roi_flow_aligned_temporal_mae_8bit': float(np.abs(sampled[mask]-reference[mask]).mean())}
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
