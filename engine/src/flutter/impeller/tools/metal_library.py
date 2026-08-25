# Copyright 2013 The Flutter Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import sys

import argparse
import errno
import os
import subprocess


def make_directories(path):
  try:
    os.makedirs(path)
  except OSError as exc:
    if exc.errno == errno.EEXIST and os.path.isdir(path):
      pass
    else:
      raise


# Metal 3.0 unified the language standards: there is no longer a per-platform
# spelling, and each standard carries its own minimum deployment target.
def is_metal3(version):
  return tuple(int(part) for part in version.split('.')) >= (3, 0)


def language_standard(platform, version):
  if is_metal3(version):
    return '--std=metal%s' % version
  if platform == 'mac':
    return '--std=macos-metal%s' % version
  return '--std=ios-metal%s' % version


def deployment_target(platform, version):
  metal3 = is_metal3(version)
  if platform == 'mac':
    return '-mmacos-version-min=%s' % ('13.0' if metal3 else '10.14')
  if platform == 'ios':
    return '-mios-version-min=%s' % ('16.0' if metal3 else '11.0')
  return '-miphonesimulator-version-min=%s' % ('16.0' if metal3 else '11.0')


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument(
      '--output', type=str, required=True, help='The location to generate the Metal library to.'
  )
  parser.add_argument('--depfile', type=str, required=True, help='The location of the depfile.')
  parser.add_argument(
      '--source',
      type=str,
      action='append',
      required=True,
      help='The source file to compile. Can be specified multiple times.'
  )
  parser.add_argument(
      '--platform',
      required=True,
      choices=['mac', 'ios', 'ios-simulator'],
      help='Select the platform.'
  )
  parser.add_argument(
      '--metal-version', required=True, help='The language standard version to compile for.'
  )
  parser.add_argument('--debug', action='store_true', help='Generate debugging information.')

  args = parser.parse_args()

  make_directories(os.path.dirname(args.depfile))

  command = [
      'xcrun',
  ]

  # Select the SDK.
  command += ['-sdk']
  if args.platform == 'mac':
    command += [
        'macosx',
    ]
  elif args.platform == 'ios':
    command += [
        'iphoneos',
    ]
  elif args.platform == 'ios-simulator':
    command += [
        'iphonesimulator',
    ]
  else:
    raise 'Unknown target platform'

  command += [
      'metal',
      # These warnings are from generated code and would make no sense to the
      # GLSL author.
      '-Wno-unused-variable',
      # Both user and system header will be tracked.
      '-MMD',
      # Like -Os (and thus -O2), but reduces code size further.
      '-Oz',
      # Allow aggressive, lossy floating-point optimizations.
      '-ffast-math',
      '-MF',
      args.depfile,
      '-o',
      args.output,
  ]

  if args.debug:
    command += [
        '-g',
        '-frecord-sources',
    ]
  else:
    command += [
        # Record symbols in a separate *.metallibsym file.
        '-frecord-sources=flat',
    ]

  # Select the Metal standard and the minimum supported OS versions.
  # The Metal standard must match the specification in impellerc.
  command += [
      language_standard(args.platform, args.metal_version),
      deployment_target(args.platform, args.metal_version),
  ]

  command += args.source

  try:
    subprocess.check_output(command, stderr=subprocess.STDOUT, text=True)
  except subprocess.CalledProcessError as cpe:
    print(cpe.output)
    return cpe.returncode

  return 0


if __name__ == '__main__':
  if sys.platform != 'darwin':
    raise Exception('This script only runs on Mac')
  sys.exit(main())
