// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:io' as io;

import 'package:file/memory.dart';
import 'package:file_testing/file_testing.dart';
import 'package:flutter_tools/src/artifacts.dart';
import 'package:flutter_tools/src/base/file_system.dart';
import 'package:flutter_tools/src/base/logger.dart';
import 'package:flutter_tools/src/build_system/build_system.dart';
import 'package:flutter_tools/src/build_system/targets/assets.dart';
import 'package:mockito/mockito.dart';
import 'package:platform/platform.dart';

import '../../../src/common.dart';
import '../../../src/context.dart';

void main() {
  Environment environment;
  FileSystem fileSystem;
  ProcessManager processManager;
  Logger logger;
  Artifacts artifacts;
  Platform platform;

  setUp(() {
    fileSystem = MemoryFileSystem();
    processManager = FakeProcessManager.any();
    logger = MockLogger();
    artifacts = MockArtifacts();
    platform = MockPlatform();
    environment = Environment(
      artifacts: artifacts,
      fileSystem: fileSystem,
      logger: logger,
      platform: platform,
      processManager: processManager,
      outputDir: fileSystem.currentDirectory,
      projectDir: fileSystem.currentDirectory,
      cacheDir: fileSystem.currentDirectory,
      flutterRootDir: fileSystem.currentDirectory,
    );
    environment.buildDir.createSync(recursive: true);
    fileSystem.file(fileSystem.path.join('packages', 'flutter_tools', 'lib', 'src',
        'build_system', 'targets', 'assets.dart'))
      ..createSync(recursive: true);
    fileSystem.file(fileSystem.path.join('assets', 'foo', 'bar.png'))
      ..createSync(recursive: true);
    fileSystem.file(fileSystem.path.join('assets', 'wildcard', '#bar.png'))
      ..createSync(recursive: true);
    fileSystem.file('.packages')
      ..createSync();
    fileSystem.file('pubspec.yaml')
      ..createSync()
      ..writeAsStringSync('''
name: example

flutter:
  assets:
    - assets/foo/bar.png
    - assets/wildcard/
''');
    when(platform.isWindows).thenReturn(false);
  });

  testWithoutContext('Copies files to correct asset directory', () async {
    await const CopyAssets().build(environment);

    expect(fileSystem.file(fileSystem.path.join(environment.buildDir.path, 'flutter_assets', 'AssetManifest.json')), exists);
    expect(fileSystem.file(fileSystem.path.join(environment.buildDir.path, 'flutter_assets', 'FontManifest.json')), exists);
    expect(fileSystem.file(fileSystem.path.join(environment.buildDir.path, 'flutter_assets', 'LICENSE')), exists);
    // See https://github.com/flutter/flutter/issues/35293
    expect(fileSystem.file(fileSystem.path.join(environment.buildDir.path, 'flutter_assets', 'assets/foo/bar.png')), exists);
    // See https://github.com/flutter/flutter/issues/46163
    expect(fileSystem.file(fileSystem.path.join(environment.buildDir.path, 'flutter_assets', 'assets/wildcard/%23bar.png')), exists);
  });

  testWithoutContext('Does not leave stale files in build directory', () async {
    final BuildSystem buildSystem = BuildSystem(
      artifacts: artifacts,
      fileSystem: fileSystem,
      logger: logger,
      platform: platform
    );
    await buildSystem.build(const CopyAssets(), environment);

    expect(fileSystem.file(fileSystem.path.join(environment.buildDir.path, 'flutter_assets', 'assets/foo/bar.png')), exists);
    // Modify manifest to remove asset.
    fileSystem.file('pubspec.yaml')
      ..createSync()
      ..writeAsStringSync('''
name: example

flutter:
''');
    await buildSystem.build(const CopyAssets(), environment);

    // See https://github.com/flutter/flutter/issues/35293
    expect(fileSystem.file(fileSystem.path.join(environment.buildDir.path, 'flutter_assets', 'assets/foo/bar.png')), isNot(exists));
  }, skip: io.Platform.isWindows); // See https://github.com/google/file.dart/issues/131
}

class MockLogger extends Mock implements Logger {}
class MockArtifacts extends Mock implements Artifacts {}
class MockPlatform extends Mock implements Platform {}
