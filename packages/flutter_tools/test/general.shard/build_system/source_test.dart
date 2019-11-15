// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:flutter_tools/src/artifacts.dart';
import 'package:flutter_tools/src/base/file_system.dart';
import 'package:flutter_tools/src/base/platform.dart';
import 'package:flutter_tools/src/build_info.dart';
import 'package:flutter_tools/src/build_system/build_system.dart';
import 'package:flutter_tools/src/build_system/exceptions.dart';
import 'package:flutter_tools/src/build_system/source.dart';
import 'package:flutter_tools/src/cache.dart';
import 'package:mockito/mockito.dart';

import '../../src/common.dart';
import '../../src/testbed.dart';

void main() {
  SourceVisitor visitor;
  Environment environment;
  MockPlatform mockPlatform;
  final Testbed testbed = Testbed(setup: () {
    mockPlatform = MockPlatform();
    when(mockPlatform.isWindows).thenReturn(true);
    fs.directory('cache').createSync();
    final Directory outputs = fs.directory('outputs')
        ..createSync();
    environment = Environment(
      outputDir: outputs,
      projectDir: fs.currentDirectory,
      buildDir: fs.directory('build'),
    );
    visitor = SourceVisitor(environment);
    environment.buildDir.createSync(recursive: true);
  });

  testbed.test('configures implicit vs explict correctly', () {
    expect(const Source.pattern('{PROJECT_DIR}/foo').implicit, false);
    expect(const Source.pattern('{PROJECT_DIR}/*foo').implicit, true);
  });

  testbed.test('can substitute {PROJECT_DIR}/foo', () {
    fs.file('foo').createSync();
    const Source fooSource = Source.pattern('{PROJECT_DIR}/foo');
    fooSource.accept(visitor);

    expect(visitor.sources.single.path, fs.path.absolute('foo'));
  });

  testbed.test('can substitute {OUTPUT_DIR}/foo', () {
    fs.file('foo').createSync();
    const Source fooSource = Source.pattern('{OUTPUT_DIR}/foo');
    fooSource.accept(visitor);

    expect(visitor.sources.single.path, fs.path.absolute(fs.path.join('outputs', 'foo')));
  });


  testbed.test('can substitute {BUILD_DIR}/bar', () {
    final String path = fs.path.join(environment.buildDir.path, 'bar');
    fs.file(path).createSync();
    const Source barSource = Source.pattern('{BUILD_DIR}/bar');
    barSource.accept(visitor);

    expect(visitor.sources.single.path, fs.path.absolute(path));
  });

  testbed.test('can substitute {FLUTTER_ROOT}/foo', () {
    final String path = fs.path.join(environment.flutterRootDir.path, 'foo');
    fs.file(path).createSync();
    const Source barSource = Source.pattern('{FLUTTER_ROOT}/foo');
    barSource.accept(visitor);

    expect(visitor.sources.single.path, fs.path.absolute(path));
  });

  testbed.test('can substitute Artifact', () {
    final String path = fs.path.join(
      Cache.instance.getArtifactDirectory('engine').path,
      'windows-x64',
      'foo',
    );
    fs.file(path).createSync(recursive: true);
    const Source fizzSource = Source.artifact(Artifact.windowsDesktopPath, platform: TargetPlatform.windows_x64);
    fizzSource.accept(visitor);

    expect(visitor.sources.single.resolveSymbolicLinksSync(), fs.path.absolute(path));
  });

  testbed.test('can substitute {PROJECT_DIR}/*.fizz', () {
    const Source fizzSource = Source.pattern('{PROJECT_DIR}/*.fizz');
    fizzSource.accept(visitor);

    expect(visitor.sources, isEmpty);

    fs.file('foo.fizz').createSync();
    fs.file('foofizz').createSync();


    fizzSource.accept(visitor);

    expect(visitor.sources.single.path, fs.path.absolute('foo.fizz'));
  });

  testbed.test('can substitute {PROJECT_DIR}/fizz.*', () {
    const Source fizzSource = Source.pattern('{PROJECT_DIR}/fizz.*');
    fizzSource.accept(visitor);

    expect(visitor.sources, isEmpty);

    fs.file('fizz.foo').createSync();
    fs.file('fizz').createSync();

    fizzSource.accept(visitor);

    expect(visitor.sources.single.path, fs.path.absolute('fizz.foo'));
  });


  testbed.test('can substitute {PROJECT_DIR}/a*bc', () {
    const Source fizzSource = Source.pattern('{PROJECT_DIR}/bc*bc');
    fizzSource.accept(visitor);

    expect(visitor.sources, isEmpty);

    fs.file('bcbc').createSync();
    fs.file('bc').createSync();

    fizzSource.accept(visitor);

    expect(visitor.sources.single.path, fs.path.absolute('bcbc'));
  });


  testbed.test('crashes on bad substitute of two **', () {
    const Source fizzSource = Source.pattern('{PROJECT_DIR}/*.*bar');

    fs.file('abcd.bar').createSync();

    expect(() => fizzSource.accept(visitor), throwsA(isInstanceOf<InvalidPatternException>()));
  });


  testbed.test('can\'t substitute foo', () {
    const Source invalidBase = Source.pattern('foo');

    expect(() => invalidBase.accept(visitor), throwsA(isInstanceOf<InvalidPatternException>()));
  });

  testbed.test('can substitute optional files', () {
    const Source missingSource = Source.pattern('{PROJECT_DIR}/foo', optional: true);

    expect(fs.file('foo').existsSync(), false);
    missingSource.accept(visitor);
    expect(visitor.sources, isEmpty);
  });

  testbed.test('can resolve a missing depfile', () {
    const Source depfile = Source.depfile('foo.d');

    depfile.accept(visitor);
    expect(visitor.sources, isEmpty);
    expect(visitor.containsNewDepfile, true);
  });

  testbed.test('can resolve a populated depfile', () {
    const Source depfile = Source.depfile('foo.d');
    environment.buildDir.childFile('foo.d')
      .writeAsStringSync('a.dart : c.dart');

    depfile.accept(visitor);
    expect(visitor.sources.single.path, 'c.dart');
    expect(visitor.containsNewDepfile, false);

    final SourceVisitor outputVisitor = SourceVisitor(environment, false);
    depfile.accept(outputVisitor);

    expect(outputVisitor.sources.single.path, 'a.dart');
    expect(outputVisitor.containsNewDepfile, false);
  });

  testbed.test('does not crash on completely invalid depfile', () {
    const Source depfile = Source.depfile('foo.d');
    environment.buildDir.childFile('foo.d')
        .writeAsStringSync('hello, world');

    depfile.accept(visitor);
    expect(visitor.sources, isEmpty);
    expect(visitor.containsNewDepfile, false);
  });

  testbed.test('can parse depfile with windows paths', () {
    const Source depfile = Source.depfile('foo.d');
    environment.buildDir.childFile('foo.d')
        .writeAsStringSync(r'a.dart: C:\\foo\\bar.txt');

    depfile.accept(visitor);
    expect(visitor.sources.single.path, r'C:\foo\bar.txt');
    expect(visitor.containsNewDepfile, false);
  }, overrides: <Type, Generator>{
    Platform: () => mockPlatform,
  });

  testbed.test('can parse depfile with spaces in paths', () {
    const Source depfile = Source.depfile('foo.d');
    environment.buildDir.childFile('foo.d')
        .writeAsStringSync(r'a.dart: foo\ bar.txt');

    depfile.accept(visitor);
    expect(visitor.sources.single.path, r'foo bar.txt');
    expect(visitor.containsNewDepfile, false);
  });
}

class MockPlatform extends Mock implements Platform {}
