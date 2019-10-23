// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:args/command_runner.dart';
import 'package:file/memory.dart';
import 'package:flutter_tools/src/base/common.dart';
import 'package:flutter_tools/src/base/file_system.dart';
import 'package:flutter_tools/src/build_system/build_system.dart';
import 'package:flutter_tools/src/build_system/targets/dart.dart';
import 'package:flutter_tools/src/cache.dart';
import 'package:flutter_tools/src/commands/build_bundle.dart';
import 'package:flutter_tools/src/features.dart';
import 'package:flutter_tools/src/reporting/reporting.dart';
import 'package:mockito/mockito.dart';
import 'package:process/process.dart';

import '../../src/common.dart';
import '../../src/context.dart';
import '../../src/testbed.dart';

void main() {
  Cache.disableLocking();
  Directory tempDir;

  setUp(() {
    tempDir = fs.systemTempDirectory.createTempSync('flutter_tools_packages_test.');
  });

  tearDown(() {
    tryToDelete(tempDir);
  });

  Future<BuildBundleCommand> runCommandIn(String projectPath, { List<String> arguments }) async {
    final BuildBundleCommand command = BuildBundleCommand();
    final CommandRunner<void> runner = createTestCommandRunner(command);
    await runner.run(<String>[
      'bundle',
      ...?arguments,
      '--target=$projectPath/lib/main.dart',
      '--no-pub'
    ]);
    return command;
  }

  testUsingContext('bundle getUsage indicate that project is a module', () async {
    final String projectPath = await createProject(tempDir,
        arguments: <String>['--no-pub', '--template=module']);

    final BuildBundleCommand command = await runCommandIn(projectPath);

    expect(await command.usageValues,
        containsPair(CustomDimensions.commandBuildBundleIsModule, 'true'));
  }, overrides: <Type, Generator>{
    BuildSystem: () => MockBuildSystem(),
  });

  testUsingContext('bundle getUsage indicate that project is not a module', () async {
    final String projectPath = await createProject(tempDir,
        arguments: <String>['--no-pub', '--template=app']);

    final BuildBundleCommand command = await runCommandIn(projectPath);

    expect(await command.usageValues,
        containsPair(CustomDimensions.commandBuildBundleIsModule, 'false'));
  }, overrides: <Type, Generator>{
    BuildSystem: () => MockBuildSystem(),
  });


  testUsingContext('bundle getUsage indicate the target platform', () async {
    final String projectPath = await createProject(tempDir,
        arguments: <String>['--no-pub', '--template=app']);

    final BuildBundleCommand command = await runCommandIn(projectPath);

    expect(await command.usageValues,
        containsPair(CustomDimensions.commandBuildBundleTargetPlatform, 'android-arm'));
  }, overrides: <Type, Generator>{
    BuildSystem: () => MockBuildSystem(),
  });


  testUsingContext('bundle fails to build for Windows if feature is disabled', () async {
    fs.file('lib/main.dart').createSync(recursive: true);
    fs.file('pubspec.yaml').createSync(recursive: true);
    fs.file('.packages').createSync(recursive: true);
    final CommandRunner<void> runner = createTestCommandRunner(BuildBundleCommand());

    expect(() => runner.run(<String>[
      'bundle',
      '--no-pub',
      '--target-platform=windows-x64',
    ]), throwsA(isInstanceOf<ToolExit>()));
  }, overrides: <Type, Generator>{
    FileSystem: () => MemoryFileSystem(),
    ProcessManager: () => FakeProcessManager(<FakeCommand>[]),
    FeatureFlags: () => TestFeatureFlags(isWindowsEnabled: false),
    BuildSystem: () => MockBuildSystem(),
  });


  testUsingContext('bundle fails to build for Linux if feature is disabled', () async {
    fs.file('lib/main.dart').createSync(recursive: true);
    fs.file('pubspec.yaml').createSync();
    fs.file('.packages').createSync();
    final CommandRunner<void> runner = createTestCommandRunner(BuildBundleCommand());

    expect(() => runner.run(<String>[
      'bundle',
      '--no-pub',
      '--target-platform=linux-x64',
    ]), throwsA(isInstanceOf<ToolExit>()));
  }, overrides: <Type, Generator>{
    FileSystem: () => MemoryFileSystem(),
    ProcessManager: () => FakeProcessManager(<FakeCommand>[]),
    FeatureFlags: () => TestFeatureFlags(isLinuxEnabled: false),
  });

  testUsingContext('bundle fails to build for macOS if feature is disabled', () async {
    fs.file('lib/main.dart').createSync(recursive: true);
    fs.file('pubspec.yaml').createSync();
    fs.file('.packages').createSync();
    final CommandRunner<void> runner = createTestCommandRunner(BuildBundleCommand());

    expect(() => runner.run(<String>[
      'bundle',
      '--no-pub',
      '--target-platform=darwin-x64',
    ]), throwsA(isInstanceOf<ToolExit>()));
  }, overrides: <Type, Generator>{
    FileSystem: () => MemoryFileSystem(),
    ProcessManager: () => FakeProcessManager(<FakeCommand>[]),
    FeatureFlags: () => TestFeatureFlags(isMacOSEnabled: false),
    BuildSystem: () => MockBuildSystem(),
  });


  testUsingContext('bundle can build for Windows if feature is enabled', () async {
    fs.file('lib/main.dart').createSync(recursive: true);
    fs.file('pubspec.yaml').createSync();
    fs.file('.packages').createSync();
    final CommandRunner<void> runner = createTestCommandRunner(BuildBundleCommand());

    await runner.run(<String>[
      'bundle',
      '--no-pub',
      '--target-platform=windows-x64',
    ]);
  }, overrides: <Type, Generator>{
    FileSystem: () => MemoryFileSystem(),
    ProcessManager: () => FakeProcessManager(<FakeCommand>[]),
    FeatureFlags: () => TestFeatureFlags(isWindowsEnabled: true),
    BuildSystem: () => MockBuildSystem(),
  });

  testUsingContext('bundle can build for Linux if feature is enabled', () async {
    fs.file('lib/main.dart').createSync(recursive: true);
    fs.file('pubspec.yaml').createSync();
    fs.file('.packages').createSync();
    final CommandRunner<void> runner = createTestCommandRunner(BuildBundleCommand());

    await runner.run(<String>[
      'bundle',
      '--no-pub',
      '--target-platform=linux-x64',
    ]);
  }, overrides: <Type, Generator>{
    FileSystem: () => MemoryFileSystem(),
    ProcessManager: () => FakeProcessManager(<FakeCommand>[]),
    FeatureFlags: () => TestFeatureFlags(isLinuxEnabled: true),
    BuildSystem: () => MockBuildSystem(),
  });


  testUsingContext('bundle can build for macOS if feature is enabled', () async {
    fs.file('lib/main.dart').createSync(recursive: true);
    fs.file('pubspec.yaml').createSync();
    fs.file('.packages').createSync();
    final CommandRunner<void> runner = createTestCommandRunner(BuildBundleCommand());

    await runner.run(<String>[
      'bundle',
      '--no-pub',
      '--target-platform=darwin-x64',
    ]);
  }, overrides: <Type, Generator>{
    FileSystem: () => MemoryFileSystem(),
    ProcessManager: () => FakeProcessManager(<FakeCommand>[]),
    FeatureFlags: () => TestFeatureFlags(isMacOSEnabled: true),
    BuildSystem: () => MockBuildSystem(),
  });


  testUsingContext('passes track widget creation through', () async {
    fs.file('lib/main.dart').createSync(recursive: true);
    fs.file('pubspec.yaml').createSync();
    fs.file('.packages').createSync();
    final CommandRunner<void> runner = createTestCommandRunner(BuildBundleCommand());
    when(buildSystem.build(any, any)).thenAnswer((Invocation invocation) async {
      final Environment environment = invocation.positionalArguments[1];
      expect(environment.defines, <String, String>{
        kTargetFile: fs.path.join('lib', 'main.dart'),
        kBuildMode: 'debug',
        kTargetPlatform: 'android-arm',
        kTrackWidgetCreation: 'true',
      });

      return BuildResult(success: true);
    });

    await runner.run(<String>[
      'bundle',
      '--no-pub',
      '--debug',
      '--target-platform=android-arm',
      '--track-widget-creation'
    ]);
  }, overrides: <Type, Generator>{
    FileSystem: () => MemoryFileSystem(),
    BuildSystem: () => MockBuildSystem(),
    ProcessManager: () => FakeProcessManager(<FakeCommand>[]),
  });
}

class MockBuildSystem extends Mock implements BuildSystem {}
