// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:file/memory.dart';
import 'package:file_testing/file_testing.dart';
import 'package:flutter_tools/src/artifacts.dart';
import 'package:flutter_tools/src/base/build.dart';
import 'package:flutter_tools/src/base/file_system.dart';
import 'package:flutter_tools/src/base/io.dart';
import 'package:flutter_tools/src/base/logger.dart';
import 'package:flutter_tools/src/build_system/build_system.dart';
import 'package:flutter_tools/src/build_system/targets/dart.dart';
import 'package:flutter_tools/src/build_system/targets/macos.dart';
import 'package:flutter_tools/src/cache.dart';
import 'package:flutter_tools/src/macos/cocoapods.dart';
import 'package:flutter_tools/src/macos/xcode.dart';
import 'package:mockito/mockito.dart';
import 'package:process/process.dart';
import 'package:platform/platform.dart';

import '../../../src/common.dart';
import '../../../src/fake_process_manager.dart';

const String _kInputPrefix = 'bin/cache/artifacts/engine/darwin-x64/FlutterMacOS.framework';
const String _kOutputPrefix = 'FlutterMacOS.framework';

final List<String> kInputs = <String>[
  '$_kInputPrefix/FlutterMacOS',
  // Headers
  '$_kInputPrefix/Headers/FlutterDartProject.h',
  '$_kInputPrefix/Headers/FlutterEngine.h',
  '$_kInputPrefix/Headers/FlutterViewController.h',
  '$_kInputPrefix/Headers/FlutterBinaryMessenger.h',
  '$_kInputPrefix/Headers/FlutterChannels.h',
  '$_kInputPrefix/Headers/FlutterCodecs.h',
  '$_kInputPrefix/Headers/FlutterMacros.h',
  '$_kInputPrefix/Headers/FlutterPluginMacOS.h',
  '$_kInputPrefix/Headers/FlutterPluginRegistrarMacOS.h',
  '$_kInputPrefix/Headers/FlutterMacOS.h',
  // Modules
  '$_kInputPrefix/Modules/module.modulemap',
  // Resources
  '$_kInputPrefix/Resources/icudtl.dat',
  '$_kInputPrefix/Resources/Info.plist',
  // Ignore Versions folder for now
  'packages/flutter_tools/lib/src/build_system/targets/macos.dart',
];

void main() {
  Environment environment;
  MockPlatform mockPlatform;
  ProcessManager processManager;
  FileSystem fileSystem;
  Artifacts artifacts;

  setUpAll(() {
    Cache.disableLocking();
    Cache.flutterRoot = '';
  });

  setUp(() {
    mockPlatform = MockPlatform();
    processManager = MockProcessManager();
    fileSystem = MemoryFileSystem();
    artifacts = MockArtifacts();

    fileSystem.file(fileSystem.path.join('bin', 'cache', 'pkg', 'sky_engine', 'lib', 'ui',
        'ui.dart')).createSync(recursive: true);
    fileSystem.file(fileSystem.path.join('bin', 'cache', 'pkg', 'sky_engine', 'sdk_ext',
        'vmservice_io.dart')).createSync(recursive: true);

    environment = Environment(
      fileSystem: fileSystem,
      processManager: processManager,
      artifacts: artifacts,
      logger: MockLogger(),
      platform: mockPlatform,
      outputDir: fileSystem.currentDirectory,
      projectDir: fileSystem.currentDirectory,
      defines: <String, String>{
        kBuildMode: 'debug',
        kTargetPlatform: 'darwin-x64',
      },
    );

    when(mockPlatform.isWindows).thenReturn(false);
    when(mockPlatform.isMacOS).thenReturn(true);
    when(mockPlatform.isLinux).thenReturn(false);
    when(mockPlatform.environment).thenReturn(const <String, String>{});

    when(artifacts.getArtifactPath(Artifact.flutterMacOSFramework, mode: anyNamed('mode')))
      .thenReturn(_kInputPrefix);
    when(artifacts.getArtifactPath(Artifact.vmSnapshotData, mode: anyNamed('mode'), platform: anyNamed('platform')))
      .thenReturn(fileSystem.path.join('bin/cache/artifacts/engine/darwin-x64/', 'vm_isolate_snapshot.bin'));
    when(artifacts.getArtifactPath(Artifact.isolateSnapshotData, mode: anyNamed('mode'), platform: anyNamed('platform')))
      .thenReturn(fileSystem.path.join('bin/cache/artifacts/engine/darwin-x64/', 'isolate_snapshot.bin'));
    when(artifacts.getArtifactPath(Artifact.genSnapshot, mode: anyNamed('mode'), platform: anyNamed('platform')))
      .thenReturn('gen_snapshot');
  });

  testWithoutContext('Copies files to correct cache directory', () async {
    for (final String input in kInputs) {
      fileSystem.file(input).createSync(recursive: true);
    }
    // Create output directory so we can test that it is deleted.
    environment.outputDir.childDirectory(_kOutputPrefix)
        .createSync(recursive: true);

    when(processManager.run(any)).thenAnswer((Invocation invocation) async {
      final List<String> arguments = invocation.positionalArguments.first as List<String>;
      final String sourcePath = arguments[arguments.length - 2];
      final String targetPath = arguments.last;
      final Directory source = fileSystem.directory(sourcePath);
      final Directory target = fileSystem.directory(targetPath);

      // verify directory was deleted by command.
      expect(target.existsSync(), false);
      target.createSync(recursive: true);

      for (final FileSystemEntity entity in source.listSync(recursive: true)) {
        if (entity is File) {
          final String relative = fileSystem.path.relative(entity.path, from: source.path);
          final String destination = fileSystem.path.join(target.path, relative);
          if (!fileSystem.file(destination).parent.existsSync()) {
            fileSystem.file(destination).parent.createSync();
          }
          entity.copySync(destination);
        }
      }
      return FakeProcessResult()..exitCode = 0;
    });
    await const DebugUnpackMacOS().build(environment);

    expect(fileSystem.directory('$_kOutputPrefix').existsSync(), true);
    for (final String input in kInputs) {
      fileSystem.file(input).createSync(recursive: true);
    }
    for (final String path in kInputs) {
      expect(fileSystem.file(path.replaceFirst(_kInputPrefix, _kOutputPrefix)), exists);
    }
  });

  testWithoutContext('debug macOS application fails if App.framework missing', () async {
    final String inputKernel = fileSystem.path.join(environment.buildDir.path, 'app.dill');
    fileSystem.file(inputKernel)
      ..createSync(recursive: true)
      ..writeAsStringSync('testing');

    expect(() async => await const DebugMacOSBundleFlutterAssets().build(environment),
        throwsA(isInstanceOf<Exception>()));
  });

  testWithoutContext('debug macOS application creates correctly structured framework', () async {
    fileSystem.file(fileSystem.path.join('bin', 'cache', 'artifacts', 'engine', 'darwin-x64',
        'vm_isolate_snapshot.bin')).createSync(recursive: true);
    fileSystem.file(fileSystem.path.join('bin', 'cache', 'artifacts', 'engine', 'darwin-x64',
        'isolate_snapshot.bin')).createSync(recursive: true);
    fileSystem.file(fileSystem.path.join(environment.buildDir.path, 'App.framework', 'App'))
        ..createSync(recursive: true);

    final String inputKernel = fileSystem.path.join(environment.buildDir.path, 'app.dill');
    final String outputKernel = fileSystem.path.join('App.framework', 'Versions', 'A', 'Resources',
        'flutter_assets', 'kernel_blob.bin');
    final String outputPlist = fileSystem.path.join('App.framework', 'Versions', 'A', 'Resources',
        'Info.plist');
    fileSystem.file(inputKernel)
      ..createSync(recursive: true)
      ..writeAsStringSync('testing');

    await const DebugMacOSBundleFlutterAssets().build(environment);

    expect(fileSystem.file(outputKernel).readAsStringSync(), 'testing');
    expect(fileSystem.file(outputPlist).readAsStringSync(), contains('io.flutter.flutter.app'));
  });

  testWithoutContext('release/profile macOS application has no blob or precompiled runtime', () async {
    fileSystem.file(fileSystem.path.join('bin', 'cache', 'artifacts', 'engine', 'darwin-x64',
        'vm_isolate_snapshot.bin')).createSync(recursive: true);
    fileSystem.file(fileSystem.path.join('bin', 'cache', 'artifacts', 'engine', 'darwin-x64',
        'isolate_snapshot.bin')).createSync(recursive: true);
    fileSystem.file(fileSystem.path.join(environment.buildDir.path, 'App.framework', 'App'))
        ..createSync(recursive: true);
    final String outputKernel = fileSystem.path.join('App.framework', 'Resources',
        'flutter_assets', 'kernel_blob.bin');
    final String precompiledVm = fileSystem.path.join('App.framework', 'Resources',
        'flutter_assets', 'vm_snapshot_data');
    final String precompiledIsolate = fileSystem.path.join('App.framework', 'Resources',
        'flutter_assets', 'isolate_snapshot_data');
    await const ProfileMacOSBundleFlutterAssets().build(environment..defines[kBuildMode] = 'profile');

    expect(fileSystem.file(outputKernel).existsSync(), false);
    expect(fileSystem.file(precompiledVm).existsSync(), false);
    expect(fileSystem.file(precompiledIsolate).existsSync(), false);
  });

  testWithoutContext('release/profile macOS application updates when App.framework updates', () async {
    fileSystem.file(fileSystem.path.join('bin', 'cache', 'artifacts', 'engine', 'darwin-x64',
        'vm_isolate_snapshot.bin')).createSync(recursive: true);
    fileSystem.file(fileSystem.path.join('bin', 'cache', 'artifacts', 'engine', 'darwin-x64',
        'isolate_snapshot.bin')).createSync(recursive: true);
    final File inputFramework = fileSystem.file(fileSystem.path.join(environment.buildDir.path, 'App.framework', 'App'))
        ..createSync(recursive: true)
        ..writeAsStringSync('ABC');

    await const ProfileMacOSBundleFlutterAssets().build(environment..defines[kBuildMode] = 'profile');
    final File outputFramework = fileSystem.file(fileSystem.path.join(environment.outputDir.path, 'App.framework', 'App'));

    expect(outputFramework.readAsStringSync(), 'ABC');

    inputFramework.writeAsStringSync('DEF');
    await const ProfileMacOSBundleFlutterAssets().build(environment..defines[kBuildMode] = 'profile');

    expect(outputFramework.readAsStringSync(), 'DEF');
  });

  testWithoutContext('release/profile macOS compilation uses correct gen_snapshot', () async {
    environment = Environment(
      fileSystem: fileSystem,
      processManager: FakeProcessManager.list(nonconst(<FakeCommand>[
        const FakeCommand(
          command: <String>[
            'gen_snapshot',
            '--causal_async_stacks',
            '--deterministic',
            '--snapshot_kind=app-aot-assembly',
            '--assembly=/build/960afb0a9fbeaf5b14a95cc437f10159/snapshot_assembly.S',
            '/build/960afb0a9fbeaf5b14a95cc437f10159/app.dill',
          ]
        ),
        const FakeCommand(
          command: <String>[
            'xcrun',
            'cc',
            '-arch',
            'x86_64',
            '-c',
            '/build/960afb0a9fbeaf5b14a95cc437f10159/snapshot_assembly.S',
            '-o',
            '/build/960afb0a9fbeaf5b14a95cc437f10159/snapshot_assembly.o',
          ]
        ),
        const FakeCommand(
          command: <String>[
            'xcrun',
            'clang',
            '-arch',
            'x86_64',
            '-dynamiclib',
            '-Xlinker',
            '-rpath',
            '-Xlinker',
            '@executable_path/Frameworks',
            '-Xlinker',
            '-rpath',
            '-Xlinker',
            '@loader_path/Frameworks',
            '-install_name',
            '@rpath/App.framework/App',
            '-o',
            '/build/960afb0a9fbeaf5b14a95cc437f10159/App.framework/App',
            '/build/960afb0a9fbeaf5b14a95cc437f10159/snapshot_assembly.o',
          ],
        )
      ])),
      artifacts: artifacts,
      logger: MockLogger(),
      platform: mockPlatform,
      outputDir: fileSystem.currentDirectory,
      projectDir: fileSystem.currentDirectory,
      defines: <String, String>{
        kBuildMode: 'debug',
        kTargetPlatform: 'darwin-x64',
      },
    );

    environment.buildDir.childFile('app.dill').createSync(recursive: true);

    await const CompileMacOSFramework().build(environment..defines[kBuildMode] = 'release');
  });
}

class MockPlatform extends Mock implements Platform {}
class MockCocoaPods extends Mock implements CocoaPods {}
class MockProcessManager extends Mock implements ProcessManager {}
class MockGenSnapshot extends Mock implements GenSnapshot {}
class MockXCode extends Mock implements Xcode {}
class MockArtifacts extends Mock implements Artifacts {}
class MockLogger extends Mock implements Logger {}
class FakeProcessResult implements ProcessResult {
  @override
  int exitCode;

  @override
  int pid = 0;

  @override
  String stderr = '';

  @override
  String stdout = '';
}

// Work-around for silly lint check.
T nonconst<T>(T input) => input;
