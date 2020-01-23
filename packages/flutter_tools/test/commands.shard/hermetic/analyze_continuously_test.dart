// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';

import 'package:flutter_tools/src/base/file_system.dart';
import 'package:flutter_tools/src/base/os.dart';
import 'package:flutter_tools/src/dart/analysis.dart';
import 'package:flutter_tools/src/dart/pub.dart';
import 'package:flutter_tools/src/dart/sdk.dart';
import 'package:flutter_tools/src/runner/flutter_command_runner.dart';
import 'package:flutter_tools/src/globals.dart' as globals;

import '../../src/common.dart';
import '../../src/context.dart';

void main() {
  AnalysisServer server;
  Directory tempDir;

  setUp(() {
    FlutterCommandRunner.initFlutterRoot();
    tempDir = globals.fs.systemTempDirectory.createTempSync('flutter_analysis_test.');
  });

  tearDown(() {
    tryToDelete(tempDir);
    return server?.dispose();
  });

  group('analyze --watch', () {
    testUsingContext('AnalysisServer success', () async {
      _createSampleProject(tempDir);

      await pub.get(context: PubContext.flutterTests, directory: tempDir.path);

      server = AnalysisServer(dartSdkPath, <String>[tempDir.path]);

      int errorCount = 0;
      final Future<bool> onDone = server.onAnalyzing.where((bool analyzing) => analyzing == false).first;
      server.onErrors.listen((FileAnalysisErrors errors) => errorCount += errors.errors.length);

      await server.start();
      await onDone;

      expect(errorCount, 0);
    }, overrides: <Type, Generator>{
      OperatingSystemUtils: () => os,
      Pub: () => const Pub(),
    });
  });

  testUsingContext('AnalysisServer errors', () async {
    _createSampleProject(tempDir, brokenCode: true);

    await pub.get(context: PubContext.flutterTests, directory: tempDir.path);

    server = AnalysisServer(dartSdkPath, <String>[tempDir.path]);

    int errorCount = 0;
    final Future<bool> onDone = server.onAnalyzing.where((bool analyzing) => analyzing == false).first;
    server.onErrors.listen((FileAnalysisErrors errors) {
      errorCount += errors.errors.length;
    });

    await server.start();
    await onDone;

    expect(errorCount, greaterThan(0));
  }, overrides: <Type, Generator>{
    OperatingSystemUtils: () => os,
    Pub: () => const Pub(),
  });

  testUsingContext('Returns no errors when source is error-free', () async {
    const String contents = "StringBuffer bar = StringBuffer('baz');";
    tempDir.childFile('main.dart').writeAsStringSync(contents);
    server = AnalysisServer(dartSdkPath, <String>[tempDir.path]);

    int errorCount = 0;
    final Future<bool> onDone = server.onAnalyzing.where((bool analyzing) => analyzing == false).first;
    server.onErrors.listen((FileAnalysisErrors errors) {
      errorCount += errors.errors.length;
    });
    await server.start();
    await onDone;
    expect(errorCount, 0);
  }, overrides: <Type, Generator>{
    OperatingSystemUtils: () => os,
    Pub: () => const Pub(),
  });
}

void _createSampleProject(Directory directory, { bool brokenCode = false }) {
  final File pubspecFile = globals.fs.file(globals.fs.path.join(directory.path, 'pubspec.yaml'));
  pubspecFile.writeAsStringSync('''
name: foo_project
''');

  final File dartFile = globals.fs.file(globals.fs.path.join(directory.path, 'lib', 'main.dart'));
  dartFile.parent.createSync();
  dartFile.writeAsStringSync('''
void main() {
  print('hello world');
  ${brokenCode ? 'prints("hello world");' : ''}
}
''');
}
