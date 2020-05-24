// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:flutter_tools/src/base/io.dart';
import 'package:flutter_tools/src/base/platform.dart';
import 'package:flutter_tools/src/doctor.dart';
import 'package:flutter_tools/src/platform_validator.dart';
import 'package:mockito/mockito.dart';

import '../src/common.dart';
import '../src/fake_process_manager.dart';
void main() {
  testWithoutContext('platform validator title has operating system name in it.', () async {
    final DoctorValidator validator = PlatformValidator(
      processManager: FakeProcessManager.any(),
      platform: FakePlatform(operatingSystem: 'foobar'),
    );

    expect(validator.title, 'Platform Dependencies (foobar)');
  });

  testWithoutContext('platform validator reports git version on success', () async {
    final FakeProcessManager processManager = FakeProcessManager.list(
      <FakeCommand>[
        const FakeCommand(
          command: <String>['git', '--version'],
          stdout: 'git version 2.26.0.windows.1'
        )
      ]
    );
    final DoctorValidator validator = PlatformValidator(
      processManager: processManager,
      platform: FakePlatform(operatingSystem: 'windows'),
    );

    final ValidationResult result = await validator.validate();

    expect(result.type, ValidationType.installed);
    expect(result.messages.first.message, contains('git version 2.26.0.windows.1'));
  });

  testWithoutContext('windows platform validator reports error if git '
    '--version does not exit 0', () async {
    final FakeProcessManager processManager = FakeProcessManager.list(
      <FakeCommand>[
        const FakeCommand(
          command: <String>['git', '--version'],
          exitCode: 123,
        )
      ]
    );
    final DoctorValidator validator = PlatformValidator(
      processManager: processManager,
      platform: FakePlatform(operatingSystem: 'linux'),
    );

    final ValidationResult result = await validator.validate();

    expect(result.type, ValidationType.partial);
    expect(result.messages.last.message, contains('exit code 123'));
  });

  testWithoutContext('platform validator reports exit code 1 if git '
    '--version throws a ProcessException', () async {
    final FakeProcessManager processManager = FakeProcessManager.list(
      <FakeCommand>[
        FakeCommand(
          command: const <String>['git', '--version'],
          onRun: () {
            throw const ProcessException('git', <String>['--version']);
          }
        )
      ]
    );
    final DoctorValidator validator = PlatformValidator(
      processManager: processManager,
      platform: FakePlatform(operatingSystem: 'linux'),
    );

    final ValidationResult result = await validator.validate();

    expect(result.type, ValidationType.partial);
    expect(result.messages.last.message, contains('exit code 1'));
  });

  testWithoutContext('platform validator reports missing if git cannot '
    'be run', () async {
    final MockProcessManager processManager = MockProcessManager();
    final DoctorValidator validator = PlatformValidator(
      processManager: processManager,
      platform: FakePlatform(operatingSystem: 'linux'),
    );
    when(processManager.canRun('git')).thenReturn(false);

    final ValidationResult result = await validator.validate();

    expect(result.type, ValidationType.missing);
    expect(result.messages.last.message, contains('git for Windows is missing'));
  });

  testWithoutContext('platform validator reports missing if git cannot '
    'be run due to ArgumentError', () async {
    final MockProcessManager processManager = MockProcessManager();
    final DoctorValidator validator = PlatformValidator(
      processManager: processManager,
      platform: FakePlatform(operatingSystem: 'linux'),
    );
    when(processManager.canRun('git')).thenThrow(ArgumentError());

    final ValidationResult result = await validator.validate();

    expect(result.type, ValidationType.missing);
    expect(result.messages.last.message, contains('git for Windows is missing'));
  });
}

class MockProcessManager extends Mock implements ProcessManager {}
