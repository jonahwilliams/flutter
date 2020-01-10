// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:file/memory.dart';
import 'package:file_testing/file_testing.dart';
import 'package:flutter_tools/src/artifacts.dart';
import 'package:flutter_tools/src/base/file_system.dart';
import 'package:flutter_tools/src/base/logger.dart';

import 'package:flutter_tools/src/build_system/build_system.dart';
import 'package:flutter_tools/src/build_system/targets/windows.dart';
import 'package:flutter_tools/src/cache.dart';
import 'package:mockito/mockito.dart';
import 'package:platform/platform.dart';

import '../../../src/common.dart';
import '../../../src/fake_process_manager.dart';

const String kCachePrefix = r'C:\bin\cache\artifacts\engine\windows-x64\';

const List<String> kExpectedFiles = <String>[
  'flutter_export.h',
  'flutter_messenger.h',
  'flutter_windows.dll',
  'flutter_windows.dll.exp',
  'flutter_windows.dll.lib',
  'flutter_windows.dll.pdb',
  'flutter_export.h',
  'flutter_messenger.h',
  'flutter_plugin_registrar.h',
  'flutter_windows.h',
  'icudtl.dat',
  r'cpp_client_wrapper\foo',
];

void main() {
  BuildSystem buildSystem;
  Environment environment;
  Platform platform;
  FileSystem fileSystem;
  Logger logger;
  Artifacts artifacts;
  ProcessManager processManager;

  setUpAll(() {
    Cache.disableLocking();
    Cache.flutterRoot = '';
  });

  setUp(() {
    platform = MockPlatform();
    fileSystem = MemoryFileSystem(style: FileSystemStyle.windows);
    logger = MockLogger();
    artifacts = MockArtifacts();
    processManager = FakeProcessManager.any();

    when(platform.isWindows).thenReturn(true);
    when(platform.isMacOS).thenReturn(false);
    when(platform.isLinux).thenReturn(false);
    when(platform.pathSeparator).thenReturn(r'\');
    when(artifacts.getArtifactPath(any, mode: anyNamed('mode'), platform: anyNamed('platform')))
      .thenReturn(kCachePrefix);

    buildSystem = BuildSystem(
      fileSystem: fileSystem,
      logger: logger,
      platform: platform,
      artifacts: artifacts,
    );
    environment = Environment(
      outputDir: fileSystem.currentDirectory,
      projectDir: fileSystem.currentDirectory,
      flutterRootDir: fileSystem.currentDirectory,
      fileSystem: fileSystem,
      logger: logger,
      artifacts: artifacts,
      processManager: processManager,
      platform: platform,
    );
    for (final String path in kExpectedFiles) {
      fileSystem.file(fileSystem.path.join(kCachePrefix, path))
        ..createSync(recursive: true);
    }
    fileSystem.directory('windows').createSync();
    fileSystem.file(r'C:\packages\flutter_tools\lib\src\build_system\targets\windows.dart')
      .createSync(recursive: true);
  });

  testWithoutContext('Copies files to correct cache directory', () async {
    await buildSystem.build(const UnpackWindows(), environment);

    for (final String path in kExpectedFiles) {
      expect(fileSystem.file(fileSystem.path.join(kCachePrefix, path)), exists);
    }
  });

  testWithoutContext('Does not re-copy files unecessarily', () async {
    await buildSystem.build(const UnpackWindows(), environment);
    // Set a date in the far distant past to deal with the limited resolution
    // of the windows filesystem.
    final DateTime theDistantPast = DateTime(1991, 8, 23);
    fileSystem.file(r'C:\windows\flutter\flutter_export.h').setLastModifiedSync(theDistantPast);
    await buildSystem.build(const UnpackWindows(), environment);

    expect(fileSystem.file(r'C:\windows\flutter\flutter_export.h').statSync().modified, equals(theDistantPast));
  });

  testWithoutContext('Detects changes in input cache files', () async {
    await buildSystem.build(const UnpackWindows(), environment);
    // Set a date in the far distant past to deal with the limited resolution
    // of the windows filesystem.
    final DateTime theDistantPast = DateTime(1991, 8, 23);
    fileSystem.file(r'C:\windows\flutter\flutter_export.h').setLastModifiedSync(theDistantPast);
    final DateTime modified = fileSystem.file(r'C:\windows\flutter\flutter_export.h').statSync().modified;
    fileSystem.file(r'C:\bin\cache\artifacts\engine\windows-x64\flutter_export.h').writeAsStringSync('asd'); // modify cache.

    await buildSystem.build(const UnpackWindows(), environment);

    expect(fileSystem.file(r'C:\windows\flutter\flutter_export.h').statSync().modified, isNot(modified));
  });
}

class MockPlatform extends Mock implements Platform {}
class MockLogger extends Mock implements Logger {}
class MockArtifacts extends Mock implements Artifacts {}
