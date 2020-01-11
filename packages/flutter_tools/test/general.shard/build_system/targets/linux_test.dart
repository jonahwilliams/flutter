// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:file/memory.dart';
import 'package:file_testing/file_testing.dart';
import 'package:flutter_tools/src/artifacts.dart';
import 'package:flutter_tools/src/base/logger.dart';
import 'package:platform/platform.dart';
import 'package:flutter_tools/src/base/file_system.dart';
import 'package:flutter_tools/src/build_system/build_system.dart';
import 'package:flutter_tools/src/build_system/targets/dart.dart';
import 'package:flutter_tools/src/build_system/targets/linux.dart';
import 'package:mockito/mockito.dart';

import '../../../src/common.dart';
import '../../../src/fake_process_manager.dart';

void main() {
  Environment environment;
  MockPlatform mockPlatform;
  FileSystem fileSystem;
  Logger logger;
  Artifacts artifacts;

  setUp(() {
    fileSystem = MemoryFileSystem();
    mockPlatform = MockPlatform();
    logger = MockLogger();
    artifacts = MockArtifacts();
    environment = Environment(
      fileSystem: fileSystem,
      artifacts: artifacts,
      logger: logger,
      processManager: FakeProcessManager.any(),
      platform: mockPlatform,
      outputDir: fileSystem.currentDirectory,
      projectDir: fileSystem.currentDirectory,
      flutterRootDir: fileSystem.currentDirectory,
      cacheDir: fileSystem.currentDirectory
        .childDirectory('bin')
        .childDirectory('cache'),
      defines: <String, String>{
        kBuildMode: 'debug',
      }
    );
    environment.buildDir.createSync(recursive: true);
    fileSystem.file('bin/cache/artifacts/engine/linux-x64/unrelated-stuff').createSync(recursive: true);
    fileSystem.file('bin/cache/artifacts/engine/linux-x64/libflutter_linux_glfw.so').createSync(recursive: true);
    fileSystem.file('bin/cache/artifacts/engine/linux-x64/flutter_export.h').createSync();
    fileSystem.file('bin/cache/artifacts/engine/linux-x64/flutter_messenger.h').createSync();
    fileSystem.file('bin/cache/artifacts/engine/linux-x64/flutter_plugin_registrar.h').createSync();
    fileSystem.file('bin/cache/artifacts/engine/linux-x64/flutter_glfw.h').createSync();
    fileSystem.file('bin/cache/artifacts/engine/linux-x64/icudtl.dat').createSync();
    fileSystem.file('bin/cache/artifacts/engine/linux-x64/cpp_client_wrapper_glfw/foo').createSync(recursive: true);
    fileSystem.file('packages/flutter_tools/lib/src/build_system/targets/linux.dart').createSync(recursive: true);
    fileSystem.directory('linux').createSync();

    when(mockPlatform.isWindows).thenReturn(false);
    when(mockPlatform.isMacOS).thenReturn(false);
    when(mockPlatform.isLinux).thenReturn(true);
    when(mockPlatform.environment)
      .thenReturn(Map<String, String>.unmodifiable(<String, String>{}));
    when(artifacts.getArtifactPath(Artifact.linuxDesktopPath))
      .thenReturn('bin/cache/artifacts/engine/linux-x64/');

  });

  testWithoutContext('Copies files to correct cache directory, excluding unrelated code', () async {
    await const UnpackLinuxDebug().build(environment);

    expect(fileSystem.file('linux/flutter/ephemeral/libflutter_linux_glfw.so'), exists);
    expect(fileSystem.file('linux/flutter/ephemeral/flutter_export.h'), exists);
    expect(fileSystem.file('linux/flutter/ephemeral/flutter_messenger.h'), exists);
    expect(fileSystem.file('linux/flutter/ephemeral/flutter_plugin_registrar.h'), exists);
    expect(fileSystem.file('linux/flutter/ephemeral/flutter_glfw.h'), exists);
    expect(fileSystem.file('linux/flutter/ephemeral/icudtl.dat'), exists);
    expect(fileSystem.file('linux/flutter/ephemeral/cpp_client_wrapper_glfw/foo'), exists);
    expect(fileSystem.file('linux/flutter/ephemeral/unrelated-stuff'), isNot(exists));
  });

  testWithoutContext('Does not re-copy files unecessarily', () async {
    await const UnpackLinuxDebug().build(environment);
    // Set a date in the far distant past to deal with the limited resolution
    // of the windows filesystem.
    final DateTime theDistantPast = DateTime(1991, 8, 23);
    fileSystem.file('linux/flutter/ephemeral/libflutter_linux_glfw.so').setLastModifiedSync(theDistantPast);
    await const UnpackLinuxDebug().build(environment);

    expect(fileSystem.file('linux/flutter/ephemeral/libflutter_linux_glfw.so').statSync().modified, equals(theDistantPast));
  });

  testWithoutContext('Detects changes in input cache files', () async {
    await const UnpackLinuxDebug().build(environment);
    fileSystem.file('bin/cache/artifacts/engine/linux-x64/libflutter_linux_glfw.so').writeAsStringSync('asd'); // modify cache.

    await const UnpackLinuxDebug().build(environment);

    expect(fileSystem.file('linux/flutter/ephemeral/libflutter_linux_glfw.so').readAsStringSync(), 'asd');
  });

  testWithoutContext('Copies artifacts to out directory', () async {
    environment.buildDir.createSync(recursive: true);

    // Create input files.
    environment.buildDir.childFile('app.dill').createSync();

    await const DebugBundleLinuxAssets().build(environment);
    final Directory output = environment.outputDir
      .childDirectory('flutter_assets');

    expect(output.childFile('kernel_blob.bin'), exists);
    expect(output.childFile('FontManifest.json'), exists);
    expect(output.childFile('AssetManifest.json'), exists);
  });
}

class MockPlatform extends Mock implements Platform {}
class MockLogger extends Mock implements Logger {}
class MockArtifacts extends Mock implements Artifacts {}
