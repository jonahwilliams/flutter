// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:file/memory.dart';
import 'package:flutter_tools/src/base/file_system.dart';
import 'package:flutter_tools/src/base/platform.dart';
import 'package:flutter_tools/src/doctor.dart';
import 'package:flutter_tools/src/intellij/intellij_validator.dart';
import 'package:flutter_tools/src/ios/plist_parser.dart';
import 'package:meta/meta.dart';
import 'package:mockito/mockito.dart';
import 'package:flutter_tools/src/globals.dart' as globals;

import '../../src/common.dart';
import '../../src/context.dart';


final Generator _kNoColorOutputPlatform = () => FakePlatform(
  localeName: 'en_US.UTF-8',
  environment: <String, String>{},
  stdoutSupportsAnsi: false,
);

final Map<Type, Generator> noColorTerminalOverride = <Type, Generator>{
  Platform: _kNoColorOutputPlatform,
};

final Platform macPlatform = FakePlatform(
  operatingSystem: 'macos',
  environment: <String, String>{'HOME': '/foo/bar'}
);

void main() {
  MockPlistParser mockPlistParser;
  MemoryFileSystem fileSystem;
  MockProcessManager mockProcessManager;

  setUp(() {
    mockProcessManager = MockProcessManager();
    mockPlistParser = MockPlistParser();
    fileSystem = MemoryFileSystem.test();
  });

  testUsingContext('intellij validator', () async {
    const String installPath = '/path/to/intelliJ';
    // Uses real filesystem
    final ValidationResult result = await IntelliJValidatorTestTarget('Test', installPath, fileSystem: globals.fs).validate();
    expect(result.type, ValidationType.partial);
    expect(result.statusInfo, 'version test.test.test');
    expect(result.messages, hasLength(4));

    ValidationMessage message = result.messages
        .firstWhere((ValidationMessage m) => m.message.startsWith('IntelliJ '));
    expect(message.message, 'IntelliJ at $installPath');

    message = result.messages
        .firstWhere((ValidationMessage m) => m.message.startsWith('Dart '));
    expect(message.message, 'Dart plugin version 162.2485');

    message = result.messages
        .firstWhere((ValidationMessage m) => m.message.startsWith('Flutter '));
    expect(message.message, contains('Flutter plugin version 0.1.3'));
    expect(message.message, contains('recommended minimum version'));
  }, overrides: noColorTerminalOverride);

  testUsingContext('intellij plugins path checking on mac', () async {
    when(mockPlistParser.getValueFromFile(any, PlistParser.kCFBundleShortVersionStringKey)).thenReturn('2020.10');

    final Directory pluginsDirectory = fileSystem.directory('/foo/bar/Library/Application Support/JetBrains/TestID2020.10/plugins')
      ..createSync(recursive: true);
    final IntelliJValidatorOnMac validator = IntelliJValidatorOnMac('Test', 'TestID', '/path/to/app', fileSystem: fileSystem);
    expect(validator.plistFile, '/path/to/app/Contents/Info.plist');
    expect(validator.pluginsPath, pluginsDirectory.path);
  }, overrides: <Type, Generator>{
    Platform: () => macPlatform,
    PlistParser: () => mockPlistParser,
    FileSystem: () => fileSystem,
    ProcessManager: () => mockProcessManager,
    FileSystemUtils: () => FileSystemUtils(
      fileSystem: fileSystem,
      platform: macPlatform,
    )
  });

  testUsingContext('legacy intellij plugins path checking on mac', () async {
    when(mockPlistParser.getValueFromFile(any, PlistParser.kCFBundleShortVersionStringKey)).thenReturn('2020.10');

    final IntelliJValidatorOnMac validator = IntelliJValidatorOnMac('Test', 'TestID', '/foo', fileSystem: fileSystem);
    expect(validator.pluginsPath, '/foo/bar/Library/Application Support/TestID2020.10');
  }, overrides: <Type, Generator>{
    Platform: () => macPlatform,
    PlistParser: () => mockPlistParser,
    FileSystem: () => fileSystem,
    FileSystemUtils: () => FileSystemUtils(
      fileSystem: fileSystem,
      platform: macPlatform,
    ),
    ProcessManager: () => FakeProcessManager.any(),
  });

  testUsingContext('intellij plugins path checking on mac with override', () async {
    when(mockPlistParser.getValueFromFile(any, 'JetBrainsToolboxApp')).thenReturn('/path/to/JetBrainsToolboxApp');

    final IntelliJValidatorOnMac validator = IntelliJValidatorOnMac('Test', 'TestID', '/foo', fileSystem: fileSystem);
    expect(validator.pluginsPath, '/path/to/JetBrainsToolboxApp.plugins');
  }, overrides: <Type, Generator>{
    PlistParser: () => mockPlistParser,
    Platform: () => macPlatform,
    FileSystem: () => fileSystem,
    FileSystemUtils: () => FileSystemUtils(
      fileSystem: fileSystem,
      platform: macPlatform,
    ),
    ProcessManager: () => FakeProcessManager.any(),
  });
}


class IntelliJValidatorTestTarget extends IntelliJValidator {
  IntelliJValidatorTestTarget(String title, String installPath, {@required FileSystem fileSystem})
    : super(title, installPath, fileSystem: fileSystem);

  // Warning: requires real test data.
  @override
  String get pluginsPath => globals.fs.path.join('test', 'data', 'intellij', 'plugins');

  @override
  String get version => 'test.test.test';
}

class MockPlistParser extends Mock implements PlistParser {}
class MockProcessManager extends Mock implements ProcessManager {}
