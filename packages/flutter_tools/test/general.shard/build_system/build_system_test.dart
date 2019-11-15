// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:flutter_tools/src/base/file_system.dart';
import 'package:flutter_tools/src/base/platform.dart';
import 'package:flutter_tools/src/build_system/build_system.dart';
import 'package:flutter_tools/src/build_system/exceptions.dart';
import 'package:flutter_tools/src/cache.dart';
import 'package:flutter_tools/src/convert.dart';
import 'package:mockito/mockito.dart';

import '../../src/common.dart';
import '../../src/context.dart';
import '../../src/testbed.dart';

void main() {
  setUpAll(() {
    Cache.disableLocking();
  });

  const BuildSystem buildSystem = BuildSystem();
  Testbed testbed;
  MockPlatform mockPlatform;
  Environment environment;
  Target fooTarget;
  Target barTarget;
  Target fizzTarget;
  Target sharedTarget;
  int fooInvocations;
  int barInvocations;
  int shared;

  setUp(() {
    fooInvocations = 0;
    barInvocations = 0;
    shared = 0;
    mockPlatform = MockPlatform();
    // Keep file paths the same.
    when(mockPlatform.isWindows).thenReturn(false);

    /// Create various testing targets.
    fooTarget = TestTarget((Environment environment) async {
      environment
        .buildDir
        .childFile('out')
        ..createSync(recursive: true)
        ..writeAsStringSync('hey');
      fooInvocations++;
    })
      ..name = 'foo'
      ..inputs = const <Source>[
        Source.pattern('{PROJECT_DIR}/foo.dart'),
      ]
      ..outputs = const <Source>[
        Source.pattern('{BUILD_DIR}/out'),
      ]
      ..dependencies = <Target>[];
    barTarget = TestTarget((Environment environment) async {
      environment.buildDir
        .childFile('bar')
        ..createSync(recursive: true)
        ..writeAsStringSync('there');
      barInvocations++;
    })
      ..name = 'bar'
      ..inputs = const <Source>[
        Source.pattern('{BUILD_DIR}/out'),
      ]
      ..outputs = const <Source>[
        Source.pattern('{BUILD_DIR}/bar'),
      ]
      ..dependencies = <Target>[];
    fizzTarget = TestTarget((Environment environment) async {
      throw Exception('something bad happens');
    })
      ..name = 'fizz'
      ..inputs = const <Source>[
        Source.pattern('{BUILD_DIR}/out'),
      ]
      ..outputs = const <Source>[
        Source.pattern('{BUILD_DIR}/fizz'),
      ]
      ..dependencies = <Target>[fooTarget];
    sharedTarget = TestTarget((Environment environment) async {
      shared += 1;
    })
      ..name = 'shared'
      ..inputs = const <Source>[
        Source.pattern('{PROJECT_DIR}/foo.dart'),
      ];
    testbed = Testbed(
      setup: () {
        environment = Environment(
          outputDir: fs.currentDirectory,
          projectDir: fs.currentDirectory,
        );
        fs.file('foo.dart')
          ..createSync(recursive: true)
          ..writeAsStringSync('');
        fs.file('pubspec.yaml').createSync();
      },
      overrides: <Type, Generator>{
        Platform: () => mockPlatform,
      },
    );
  });

  testbed.test('Does not throw exception if asked to build with missing inputs', () async {
    // Delete required input file.
    fs.file('foo.dart').deleteSync();
    final BuildResult buildResult = await buildSystem.build(fooTarget, environment);

    expect(buildResult.hasException, false);
  });

  testbed.test('Does not throw exception if it does not produce a specified output', () async {
    final Target badTarget = TestTarget((Environment environment) async {})
      ..inputs = const <Source>[
        Source.pattern('{PROJECT_DIR}/foo.dart'),
      ]
      ..outputs = const <Source>[
        Source.pattern('{BUILD_DIR}/out'),
      ];
    final BuildResult result = await buildSystem.build(badTarget, environment);

    expect(result.hasException, false);
  });

  testbed.test('Saves a stamp file with inputs and outputs', () async {
    await buildSystem.build(fooTarget, environment);

    final File stampFile = fs.file(fs.path.join(environment.buildDir.path, 'foo.stamp'));
    expect(stampFile.existsSync(), true);

    final Map<String, Object> stampContents = json.decode(stampFile.readAsStringSync());
    expect(stampContents['inputs'], <Object>['/foo.dart']);
  });

  testbed.test('Creates a BuildResult with inputs and outputs', () async {
    final BuildResult result = await buildSystem.build(fooTarget, environment);

    expect(result.inputFiles.single.path, fs.path.absolute('foo.dart'));
    expect(result.outputFiles.single.path,
        fs.path.absolute(fs.path.join(environment.buildDir.path, 'out')));
  });

  testbed.test('Does not re-invoke build if stamp is valid', () async {
    await buildSystem.build(fooTarget, environment);
    await buildSystem.build(fooTarget, environment);

    expect(fooInvocations, 1);
  });

  testbed.test('Re-invoke build if input is modified', () async {
    await buildSystem.build(fooTarget, environment);

    fs.file('foo.dart').writeAsStringSync('new contents');

    await buildSystem.build(fooTarget, environment);
    expect(fooInvocations, 2);
  });

  testbed.test('does not re-invoke build if input timestamp changes', () async {
    await buildSystem.build(fooTarget, environment);

    fs.file('foo.dart').writeAsStringSync('');

    await buildSystem.build(fooTarget, environment);
    expect(fooInvocations, 1);
  });

  testbed.test('does not re-invoke build if output timestamp changes', () async {
    await buildSystem.build(fooTarget, environment);

    environment.buildDir.childFile('out').writeAsStringSync('hey');

    await buildSystem.build(fooTarget, environment);
    expect(fooInvocations, 1);
  });


  testbed.test('Re-invoke build if output is modified', () async {
    await buildSystem.build(fooTarget, environment);

    environment.buildDir.childFile('out').writeAsStringSync('Something different');

    await buildSystem.build(fooTarget, environment);
    expect(fooInvocations, 2);
  });

  testbed.test('Runs dependencies of targets', () async {
    barTarget.dependencies.add(fooTarget);

    await buildSystem.build(barTarget, environment);

    expect(fs.file(fs.path.join(environment.buildDir.path, 'bar')).existsSync(), true);
    expect(fooInvocations, 1);
    expect(barInvocations, 1);
  });

  testbed.test('Only invokes shared dependencies once', () async {
    fooTarget.dependencies.add(sharedTarget);
    barTarget.dependencies.add(sharedTarget);
    barTarget.dependencies.add(fooTarget);

    await buildSystem.build(barTarget, environment);

    expect(shared, 1);
  });

  testbed.test('Automatically cleans old outputs when dag changes', () async {
    final TestTarget testTarget = TestTarget((Environment envionment) async {
      environment.buildDir.childFile('foo.out').createSync();
    })
      ..inputs = const <Source>[Source.pattern('{PROJECT_DIR}/foo.dart')]
      ..outputs = const <Source>[Source.pattern('{BUILD_DIR}/foo.out')];
    fs.file('foo.dart').createSync();

    await buildSystem.build(testTarget, environment);

    expect(environment.buildDir.childFile('foo.out').existsSync(), true);

    final TestTarget testTarget2 = TestTarget((Environment envionment) async {
      environment.buildDir.childFile('bar.out').createSync();
    })
      ..inputs = const <Source>[Source.pattern('{PROJECT_DIR}/foo.dart')]
      ..outputs = const <Source>[Source.pattern('{BUILD_DIR}/bar.out')];

    await buildSystem.build(testTarget2, environment);

    expect(environment.buildDir.childFile('bar.out').existsSync(), true);
    expect(environment.buildDir.childFile('foo.out').existsSync(), false);
  });

  testbed.test('Does not crash when filesytem and cache are out of sync', () async {
    final TestTarget testTarget = TestTarget((Environment environment) async {
      environment.buildDir.childFile('foo.out').createSync();
    })
      ..inputs = const <Source>[Source.pattern('{PROJECT_DIR}/foo.dart')]
      ..outputs = const <Source>[Source.pattern('{BUILD_DIR}/foo.out')];
    fs.file('foo.dart').createSync();

    await buildSystem.build(testTarget, environment);

    expect(environment.buildDir.childFile('foo.out').existsSync(), true);
    environment.buildDir.childFile('foo.out').deleteSync();

    final TestTarget testTarget2 = TestTarget((Environment environment) async {
      environment.buildDir.childFile('bar.out').createSync();
    })
      ..inputs = const <Source>[Source.pattern('{PROJECT_DIR}/foo.dart')]
      ..outputs = const <Source>[Source.pattern('{BUILD_DIR}/bar.out')];

    await buildSystem.build(testTarget2, environment);

    expect(environment.buildDir.childFile('bar.out').existsSync(), true);
    expect(environment.buildDir.childFile('foo.out').existsSync(), false);
  });

  testbed.test('reruns build if stamp is corrupted', () async {
    final TestTarget testTarget = TestTarget((Environment envionment) async {
      environment.buildDir.childFile('foo.out').createSync();
    })
      ..inputs = const <Source>[Source.pattern('{PROJECT_DIR}/foo.dart')]
      ..outputs = const <Source>[Source.pattern('{BUILD_DIR}/foo.out')];
    fs.file('foo.dart').createSync();
    await buildSystem.build(testTarget, environment);

    // invalid JSON
    environment.buildDir.childFile('test.stamp').writeAsStringSync('{X');
    await buildSystem.build(testTarget, environment);

    // empty file
    environment.buildDir.childFile('test.stamp').writeAsStringSync('');
    await buildSystem.build(testTarget, environment);

    // invalid format
    environment.buildDir.childFile('test.stamp').writeAsStringSync('{"inputs": 2, "outputs": 3}');
    await buildSystem.build(testTarget, environment);
  });


  testbed.test('handles a throwing build action', () async {
    final BuildResult result = await buildSystem.build(fizzTarget, environment);

    expect(result.hasException, true);
  });

  testbed.test('Can describe itself with JSON output', () {
    environment.buildDir.createSync(recursive: true);
    expect(fooTarget.toJson(environment), <String, dynamic>{
      'inputs':  <Object>[
        '/foo.dart',
      ],
      'outputs': <Object>[
        fs.path.join(environment.buildDir.path, 'out'),
      ],
      'dependencies': <Object>[],
      'name':  'foo',
      'stamp': fs.path.join(environment.buildDir.path, 'foo.stamp'),
    });
  });

  testbed.test('Can find dependency cycles', () {
    final Target barTarget = TestTarget()..name = 'bar';
    final Target fooTarget = TestTarget()..name = 'foo';
    barTarget.dependencies.add(fooTarget);
    fooTarget.dependencies.add(barTarget);

    expect(() => checkCycles(barTarget), throwsA(isInstanceOf<CycleException>()));
  });

  testbed.test('Target with depfile dependency will not run twice without invalidation', () async {
    int called = 0;
    final TestTarget target = TestTarget((Environment environment) async {
      environment.buildDir.childFile('example.d')
        .writeAsStringSync('a.txt: b.txt');
      fs.file('a.txt').writeAsStringSync('a');
      called += 1;
    })
      ..inputs = const <Source>[Source.depfile('example.d')]
      ..outputs = const <Source>[Source.depfile('example.d')];
    fs.file('b.txt').writeAsStringSync('b');

    await buildSystem.build(target, environment);

    expect(fs.file('a.txt').existsSync(), true);
    expect(called, 1);

    // Second build is up to date due to depfil parse.
    await buildSystem.build(target, environment);
    expect(called, 1);
  });

  testbed.test('output directory is an input to the build',  () async {
    final Environment environmentA = Environment(projectDir: fs.currentDirectory, outputDir: fs.directory('a'));
    final Environment environmentB = Environment(projectDir: fs.currentDirectory, outputDir: fs.directory('b'));

    expect(environmentA.buildDir.path, isNot(environmentB.buildDir.path));
  });

  testbed.test('A target with depfile dependencies can delete stale outputs on the first run',  () async {
    int called = 0;
    final TestTarget target = TestTarget((Environment environment) async {
      if (called == 0) {
        environment.buildDir.childFile('example.d')
          .writeAsStringSync('a.txt c.txt: b.txt');
        fs.file('a.txt').writeAsStringSync('a');
        fs.file('c.txt').writeAsStringSync('a');
      } else {
        // On second run, we no longer claim c.txt as an output.
        environment.buildDir.childFile('example.d')
          .writeAsStringSync('a.txt: b.txt');
        fs.file('a.txt').writeAsStringSync('a');
      }
      called += 1;
    })
      ..inputs = const <Source>[Source.depfile('example.d')]
      ..outputs = const <Source>[Source.depfile('example.d')];
    fs.file('b.txt').writeAsStringSync('b');

    await buildSystem.build(target, environment);

    expect(fs.file('a.txt').existsSync(), true);
    expect(fs.file('c.txt').existsSync(), true);
    expect(called, 1);

    // rewrite an input to force a rerun, espect that the old c.txt is deleted.
    fs.file('b.txt').writeAsStringSync('ba');
    await buildSystem.build(target, environment);

    expect(fs.file('a.txt').existsSync(), true);
    expect(fs.file('c.txt').existsSync(), false);
    expect(called, 2);
  });
}

class MockPlatform extends Mock implements Platform {}

// Work-around for silly lint check.
T nonconst<T>(T input) => input;

class TestTarget extends Target {
  TestTarget([this._build]);

  final Future<void> Function(Environment environment) _build;

  @override
  Future<void> build(Environment environment) => _build(environment);

  @override
  List<Target> dependencies = <Target>[];

  @override
  List<Source> inputs = <Source>[];

  @override
  String name = 'test';

  @override
  List<Source> outputs = <Source>[];
}
