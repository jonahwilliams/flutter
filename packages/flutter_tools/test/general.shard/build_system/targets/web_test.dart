// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:file/memory.dart';
import 'package:flutter_tools/src/artifacts.dart';
import 'package:flutter_tools/src/base/file_system.dart';
import 'package:flutter_tools/src/base/logger.dart';
import 'package:flutter_tools/src/build_system/build_system.dart';
import 'package:flutter_tools/src/build_system/depfile.dart';
import 'package:flutter_tools/src/build_system/targets/dart.dart';
import 'package:flutter_tools/src/build_system/targets/web.dart';
import 'package:flutter_tools/src/dart/package_map.dart';
import 'package:mockito/mockito.dart';
import 'package:process/process.dart';
import 'package:platform/platform.dart';

import '../../../src/common.dart';
import '../../../src/mocks.dart';

void main() {
  Environment environment;
  MockPlatform mockPlatform;
  MockPlatform  mockWindowsPlatform;
  FileSystem fileSystem;
  Logger logger;
  Artifacts artifacts;
  ProcessManager processManager;

  setUp(() {
    mockPlatform = MockPlatform();
    mockWindowsPlatform = MockPlatform();
    logger = MockLogger();
    artifacts = MockArtifacts();
    fileSystem = MemoryFileSystem();
    processManager = MockProcessManager();

    when(mockPlatform.isWindows).thenReturn(false);
    when(mockPlatform.isMacOS).thenReturn(true);
    when(mockPlatform.isLinux).thenReturn(false);
    when(mockPlatform.environment).thenReturn(const <String, String>{});

    when(mockWindowsPlatform.isWindows).thenReturn(true);
    when(mockWindowsPlatform.isMacOS).thenReturn(false);
    when(mockWindowsPlatform.isLinux).thenReturn(false);

    when(artifacts.getArtifactPath(Artifact.flutterWebSdk)).thenReturn(
      fileSystem.path.join('bin', 'cache', 'flutter_web_sdk'),
    );
    when(artifacts.getArtifactPath(Artifact.engineDartBinary)).thenReturn(
      fileSystem.path.join('bin', 'cache', 'dart-sdk', 'bin', 'dart'),
    );
    when(artifacts.getArtifactPath(Artifact.dart2jsSnapshot)).thenReturn(
      fileSystem.path.join('bin', 'cache', 'dart-sdk', 'bin', 'snapshots', 'dart2js.dart.snapshot')
    );

    final File packagesFile = fileSystem.file(fileSystem.path.join('foo', '.packages'))
      ..createSync(recursive: true)
      ..writeAsStringSync('foo:lib/\n');
    PackageMap.globalPackagesPath = packagesFile.path;
    fileSystem.currentDirectory.childDirectory('bar').createSync();

    environment = Environment(
      fileSystem: fileSystem,
      artifacts: artifacts,
      logger: logger,
      processManager: processManager,
      platform: mockPlatform,
      projectDir: fileSystem.currentDirectory.childDirectory('foo'),
      outputDir: fileSystem.currentDirectory.childDirectory('bar'),
      buildDir: fileSystem.currentDirectory,
      defines: <String, String>{
        kTargetFile: fileSystem.path.join('foo', 'lib', 'main.dart'),
      },
    );
    environment.buildDir.createSync(recursive: true);
  });

  testWithoutContext('WebEntrypointTarget generates an entrypoint with plugins and init platform', () async {
    environment.defines[kHasWebPlugins] = 'true';
    environment.defines[kInitializePlatform] = 'true';
    await const WebEntrypointTarget().build(environment);

    final String generated = environment.buildDir.childFile('main.dart').readAsStringSync();

    // Plugins
    expect(generated, contains("import 'package:foo/generated_plugin_registrant.dart';"));
    expect(generated, contains('registerPlugins(webPluginRegistry);'));

    // Platform
    expect(generated, contains('if (true) {'));

    // Main
    expect(generated, contains('entrypoint.main();'));

    // Import.
    expect(generated, contains("import 'package:foo/main.dart' as entrypoint;"));
  });

  testWithoutContext('WebReleaseBundle copies dart2js output and resource files to output directory', () async {
    final Directory webResources = environment.projectDir.childDirectory('web');
    webResources.childFile('index.html')
      ..createSync(recursive: true);
    webResources.childFile('foo.txt')
      ..writeAsStringSync('A');
    environment.buildDir.childFile('main.dart.js').createSync();

    await const WebReleaseBundle().build(environment);

    expect(environment.outputDir.childFile('foo.txt')
      .readAsStringSync(), 'A');
    expect(environment.outputDir.childFile('main.dart.js')
      .existsSync(), true);
    expect(environment.outputDir.childDirectory('assets')
      .childFile('AssetManifest.json').existsSync(), true);

    // Update to arbitary resource file triggers rebuild.
    webResources.childFile('foo.txt').writeAsStringSync('B');

    await const WebReleaseBundle().build(environment);

    expect(environment.outputDir.childFile('foo.txt')
      .readAsStringSync(), 'B');
  });

  testWithoutContext('WebEntrypointTarget generates an entrypoint for a file outside of main', () async {
    environment.defines[kTargetFile] = fileSystem.path.join('other', 'lib', 'main.dart');
    await const WebEntrypointTarget().build(environment);

    final String generated = environment.buildDir.childFile('main.dart').readAsStringSync();

    // Import.
    expect(generated, contains("import 'file:///other/lib/main.dart' as entrypoint;"));
  });

  testWithoutContext('WebEntrypointTarget generates an entrypoint with plugins and init platform on windows', () async {
    fileSystem =  MemoryFileSystem(style: FileSystemStyle.windows);
    final File packagesFile = fileSystem.file(fileSystem.path.join('foo', '.packages'))
      ..createSync(recursive: true)
      ..writeAsStringSync('foo:lib/\n');
    PackageMap.globalPackagesPath = packagesFile.path;
    environment = Environment(
      fileSystem: fileSystem,
      artifacts: artifacts,
      logger: logger,
      processManager: processManager,
      platform: mockWindowsPlatform,
      projectDir: fileSystem.currentDirectory.childDirectory('foo'),
      outputDir: fileSystem.currentDirectory.childDirectory('bar'),
      buildDir: fileSystem.currentDirectory,
      defines: <String, String>{
        kTargetFile: fileSystem.path.join('foo', 'lib', 'main.dart'),
      },
    );
    environment.buildDir.createSync(recursive: true);

    environment.defines[kHasWebPlugins] = 'true';
    environment.defines[kInitializePlatform] = 'true';
    await const WebEntrypointTarget().build(environment);

    final String generated = environment.buildDir.childFile('main.dart')
      .readAsStringSync();

    // Plugins
    expect(generated, contains("import 'package:foo/generated_plugin_registrant.dart';"));
    expect(generated, contains('registerPlugins(webPluginRegistry);'));

    // Platform
    expect(generated, contains('if (true) {'));

    // Main
    expect(generated, contains('entrypoint.main();'));

    // Import.
    expect(generated, contains("import 'package:foo/main.dart' as entrypoint;"));
  });

  testWithoutContext('WebEntrypointTarget generates an entrypoint without plugins and init platform', () async {
    environment.defines[kHasWebPlugins] = 'false';
    environment.defines[kInitializePlatform] = 'true';
    await const WebEntrypointTarget().build(environment);

    final String generated = environment.buildDir.childFile('main.dart').readAsStringSync();

    // Plugins
    expect(generated, isNot(contains("import 'package:foo/generated_plugin_registrant.dart';")));
    expect(generated, isNot(contains('registerPlugins(webPluginRegistry);')));

    // Platform
    expect(generated, contains('if (true) {'));

    // Main
    expect(generated, contains('entrypoint.main();'));
  });

  testWithoutContext('WebEntrypointTarget generates an entrypoint with plugins and without init platform', () async {
    environment.defines[kHasWebPlugins] = 'true';
    environment.defines[kInitializePlatform] = 'false';
    await const WebEntrypointTarget().build(environment);

    final String generated = environment.buildDir.childFile('main.dart').readAsStringSync();

    // Plugins
    expect(generated, contains("import 'package:foo/generated_plugin_registrant.dart';"));
    expect(generated, contains('registerPlugins(webPluginRegistry);'));

    // Platform
    expect(generated, contains('if (false) {'));

    // Main
    expect(generated, contains('entrypoint.main();'));
  });

  testWithoutContext('WebEntrypointTarget generates an entrypoint without plugins and without init platform', () async {
    environment.defines[kHasWebPlugins] = 'false';
    environment.defines[kInitializePlatform] = 'false';
    await const WebEntrypointTarget().build(environment);

    final String generated = environment.buildDir.childFile('main.dart').readAsStringSync();

    // Plugins
    expect(generated, isNot(contains("import 'package:foo/generated_plugin_registrant.dart';")));
    expect(generated, isNot(contains('registerPlugins(webPluginRegistry);')));

    // Platform
    expect(generated, contains('if (false) {'));

    // Main
    expect(generated, contains('entrypoint.main();'));
  });

  testWithoutContext('Dart2JSTarget calls dart2js with expected args with csp', () async {
    environment.defines[kBuildMode] = 'profile';
    environment.defines[kCspMode] = 'true';
    when(processManager.run(any)).thenAnswer((Invocation invocation) async {
      return FakeProcessResult(exitCode: 0);
    });
    await const Dart2JSTarget().build(environment);

    final List<String> expected = <String>[
      fileSystem.path.join('bin', 'cache', 'dart-sdk', 'bin', 'dart'),
      fileSystem.path.join('bin', 'cache', 'dart-sdk', 'bin', 'snapshots', 'dart2js.dart.snapshot'),
      '--libraries-spec=' + fileSystem.path.join('bin', 'cache', 'flutter_web_sdk', 'libraries.json'),
      '-O4', // highest optimizations
      '--no-minify', // but uses unminified names for debugging
      '-o',
      environment.buildDir.childFile('main.dart.js').absolute.path,
      '--packages=${fileSystem.path.join('foo', '.packages')}',
      '-Ddart.vm.profile=true',
      '--csp',
      environment.buildDir.childFile('main.dart').absolute.path,
    ];
    verify(processManager.run(expected)).called(1);
  });

  testWithoutContext('Dart2JSTarget calls dart2js with expected args in profile mode', () async {
    environment.defines[kBuildMode] = 'profile';
    when(processManager.run(any)).thenAnswer((Invocation invocation) async {
      return FakeProcessResult(exitCode: 0);
    });
    await const Dart2JSTarget().build(environment);

    final List<String> expected = <String>[
      fileSystem.path.join('bin', 'cache', 'dart-sdk', 'bin', 'dart'),
      fileSystem.path.join('bin', 'cache', 'dart-sdk', 'bin', 'snapshots', 'dart2js.dart.snapshot'),
      '--libraries-spec=' + fileSystem.path.join('bin', 'cache', 'flutter_web_sdk', 'libraries.json'),
      '-O4', // highest optimizations
      '--no-minify', // but uses unminified names for debugging
      '-o',
      environment.buildDir.childFile('main.dart.js').absolute.path,
      '--packages=${fileSystem.path.join('foo', '.packages')}',
      '-Ddart.vm.profile=true',
      environment.buildDir.childFile('main.dart').absolute.path,
    ];
    verify(processManager.run(expected)).called(1);
  });

  testWithoutContext('Dart2JSTarget calls dart2js with expected args in release mode', () async {
    environment.defines[kBuildMode] = 'release';
    when(processManager.run(any)).thenAnswer((Invocation invocation) async {
      return FakeProcessResult(exitCode: 0);
    });
    await const Dart2JSTarget().build(environment);

    final List<String> expected = <String>[
      fileSystem.path.join('bin', 'cache', 'dart-sdk', 'bin', 'dart'),
      fileSystem.path.join('bin', 'cache', 'dart-sdk', 'bin', 'snapshots', 'dart2js.dart.snapshot'),
      '--libraries-spec=' + fileSystem.path.join('bin', 'cache', 'flutter_web_sdk', 'libraries.json'),
      '-O4', // highest optimizations.
      '-o',
      environment.buildDir.childFile('main.dart.js').absolute.path,
      '--packages=${fileSystem.path.join('foo', '.packages')}',
      '-Ddart.vm.product=true',
      environment.buildDir.childFile('main.dart').absolute.path,
    ];
    verify(processManager.run(expected)).called(1);
  });

  testWithoutContext('Dart2JSTarget calls dart2js with expected args in release with dart2js optimization override', () async {
    environment.defines[kBuildMode] = 'release';
    environment.defines[kDart2jsOptimization] = 'O3';
    when(processManager.run(any)).thenAnswer((Invocation invocation) async {
      return FakeProcessResult(exitCode: 0);
    });
    await const Dart2JSTarget().build(environment);

    final List<String> expected = <String>[
      fileSystem.path.join('bin', 'cache', 'dart-sdk', 'bin', 'dart'),
      fileSystem.path.join('bin', 'cache', 'dart-sdk', 'bin', 'snapshots', 'dart2js.dart.snapshot'),
      '--libraries-spec=' + fileSystem.path.join('bin', 'cache', 'flutter_web_sdk', 'libraries.json'),
      '-O3', // configured optimizations.
      '-o',
      environment.buildDir.childFile('main.dart.js').absolute.path,
      '--packages=${fileSystem.path.join('foo', '.packages')}',
      '-Ddart.vm.product=true',
      environment.buildDir.childFile('main.dart').absolute.path,
    ];
    verify(processManager.run(expected)).called(1);
  });

  testWithoutContext('Dart2JSTarget produces expected depfile', () async {
    environment.defines[kBuildMode] = 'release';
    when(processManager.run(any)).thenAnswer((Invocation invocation) async {
      environment.buildDir.childFile('main.dart.js.deps')
        ..writeAsStringSync('file:///a.dart');
      return FakeProcessResult(exitCode: 0);
    });
    await const Dart2JSTarget().build(environment);

    expect(environment.buildDir.childFile('dart2js.d').existsSync(), true);
    final Depfile depfile = Depfile.parse(environment.buildDir.childFile('dart2js.d'));

    expect(depfile.inputs.single.path, fileSystem.path.absolute('a.dart'));
    expect(depfile.outputs.single.path,
      environment.buildDir.childFile('main.dart.js').absolute.path);
  });

  testWithoutContext('Dart2JSTarget calls dart2js with Dart defines in release mode', () async {
    environment.defines[kBuildMode] = 'release';
    environment.defines[kDartDefines] = '["FOO=bar","BAZ=qux"]';
    when(processManager.run(any)).thenAnswer((Invocation invocation) async {
      return FakeProcessResult(exitCode: 0);
    });
    await const Dart2JSTarget().build(environment);

    final List<String> expected = <String>[
      fileSystem.path.join('bin', 'cache', 'dart-sdk', 'bin', 'dart'),
      fileSystem.path.join('bin', 'cache', 'dart-sdk', 'bin', 'snapshots', 'dart2js.dart.snapshot'),
      '--libraries-spec=' + fileSystem.path.join('bin', 'cache', 'flutter_web_sdk', 'libraries.json'),
      '-O4',
      '-o',
      environment.buildDir.childFile('main.dart.js').absolute.path,
      '--packages=${fileSystem.path.join('foo', '.packages')}',
      '-Ddart.vm.product=true',
      '-DFOO=bar',
      '-DBAZ=qux',
      environment.buildDir.childFile('main.dart').absolute.path,
    ];
    verify(processManager.run(expected)).called(1);
  });

  testWithoutContext('Dart2JSTarget calls dart2js with Dart defines in profile mode', () async {
    environment.defines[kBuildMode] = 'profile';
    environment.defines[kDartDefines] = '["FOO=bar","BAZ=qux"]';
    when(processManager.run(any)).thenAnswer((Invocation invocation) async {
      return FakeProcessResult(exitCode: 0);
    });
    await const Dart2JSTarget().build(environment);

    final List<String> expected = <String>[
      fileSystem.path.join('bin', 'cache', 'dart-sdk', 'bin', 'dart'),
      fileSystem.path.join('bin', 'cache', 'dart-sdk', 'bin', 'snapshots', 'dart2js.dart.snapshot'),
      '--libraries-spec=' + fileSystem.path.join('bin', 'cache', 'flutter_web_sdk', 'libraries.json'),
      '-O4',
      '--no-minify',
      '-o',
      environment.buildDir.childFile('main.dart.js').absolute.path,
      '--packages=${fileSystem.path.join('foo', '.packages')}',
      '-Ddart.vm.profile=true',
      '-DFOO=bar',
      '-DBAZ=qux',
      environment.buildDir.childFile('main.dart').absolute.path,
    ];
    verify(processManager.run(expected)).called(1);
  });

  testWithoutContext('Dart2JSTarget throws developer-friendly exception on misformatted DartDefines', () async {
    environment.defines[kBuildMode] = 'profile';
    environment.defines[kDartDefines] = '[misformatted json';
    try {
      await const Dart2JSTarget().build(environment);
      fail('Call to build() must not have succeeded.');
    } on Exception catch(exception) {
      expect(
        '$exception',
        'Exception: The value of -D$kDartDefines is not formatted correctly.\n'
        'The value must be a JSON-encoded list of strings but was:\n'
        '[misformatted json',
      );
    }

    // Should not attempt to run any processes.
    verifyNever(processManager.run(any));
  });
}

class MockProcessManager extends Mock implements ProcessManager {}
class MockPlatform extends Mock implements Platform {}
class MockLogger extends Mock implements Logger {}
class MockArtifacts extends Mock implements Artifacts {}
