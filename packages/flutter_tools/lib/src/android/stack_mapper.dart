// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';

import 'package:process/process.dart';

import '../artifacts.dart';
import '../base/error_handling_io.dart';
import '../base/file_system.dart';
import '../base/io.dart';
import '../base/logger.dart';
import '../base/terminal.dart';

enum _MappingState {
  normal,
  header,
  backtrace,
}

/// The stack mapper runs Android native crashes though ndk-stack for local
/// engine builds of flutter.
class StackMapper {
  StackMapper({
    required LocalEngineArtifacts artifacts,
    required ProcessManager manager,
    required FileSystem fileSystem,
    required Logger logger,
    required FileSystemUtils fileSystemUtils,
  }) : _artifacts = artifacts,
       _processManager = manager,
       _fileSystem = fileSystem,
       _logger = logger,
       _fileSystemUtils = fileSystemUtils;

  final LocalEngineArtifacts _artifacts;
  final ProcessManager _processManager;
  final FileSystem _fileSystem;
  final Logger _logger;
  final FileSystemUtils _fileSystemUtils;

  static const String kCrashHeader = '*** *** *** *** ***'; // repeating a few more times...
  static const String kBackTrace = 'backtrace:';

  // Match a line like `   #06 pc`
  static final RegExp kBackTraceLine = RegExp(r'\s+#[0-9]+ pc');

  StringBuffer? _currentBuffer;
  _MappingState _state = _MappingState.normal;
  Completer<void>? _completer;

  Future<void> waitForReport() async {
    if (_completer == null) {
      if (_state == _MappingState.backtrace) {
        // logger may have exited, attempt to process backtrace
        await _processCrash(_currentBuffer.toString());
      }
      // no pending crash.
      return;
    }
    return _completer!.future;
  }

  /// Process each log line, checking if it contains the start of a crash header.
  ///
  /// If the mapper is currently processing a crash, then keep collecting lines
  /// until the end of the backtrace is reached.
  void processLine(String line) {
    switch (_state) {
      case _MappingState.normal:
        if (!line.contains(kCrashHeader)) {
          return;
        }
        _state = _MappingState.header;
        _currentBuffer = StringBuffer();
        _currentBuffer!.writeln(line);
        break;
      case _MappingState.header:
        _currentBuffer!.writeln(line);
        if (line.contains(kBackTrace)) {
          _state = _MappingState.backtrace;
        }
        break;
      case _MappingState.backtrace:
        if (line.startsWith(kBackTraceLine)) {
          _currentBuffer!.writeln(line);
        } else {
          // The end of the crash has been reached.
          _completer = Completer<void>();
          _processCrash(_currentBuffer.toString());
          _currentBuffer = null;
          _state = _MappingState.normal;
        }
        break;
    }
  }

  Future<void> _processCrash(String report) async {
    final String engineOutPath = _artifacts.engineOutPath;

    // The artifacts will usually be in:
    //   * path/to/foo/out/host_debug_unopt
    // While ndk-stack will be located in:
    //   * path/to/foo/third_party/android_tools/ndk/ndk-stack.
    final String ndkStackPath = _fileSystem.path.join(
      engineOutPath, '..', '..', 'third_party', 'android_tools', 'ndk', 'ndk-stack');
    if (!_fileSystem.file(ndkStackPath).existsSync()) {
      _logger.printTrace('Could not find ndk-stack at $ndkStackPath');
    }
    File? file;
    try {
      // Because this process often runs during shutdown, the system temp directory
      // may be deleted before ndk-stack can read the file.
      file = _fileSystem.file('crash.txt')
        ..createSync(recursive: true)
        ..writeAsStringSync(report);
      final ProcessResult result = await _processManager.run(<String>[
        ndkStackPath,
        '-sym',
        _artifacts.engineOutPath,
        '-i',
        file.path,
      ]);
      if (result.exitCode != 0) {
        _logger.printTrace('Failed to symbolize crash: ${result.stderr}${result.stdout}');
      }
      final String output = result.stdout as String;
      final File outputFile = _fileSystemUtils
        .getUniqueFile(_fileSystem.currentDirectory, 'android_crash', 'txt');
      outputFile.writeAsStringSync(output);
      _logger.printStatus('Symbolized crash written to "${outputFile.path}"', color: TerminalColor.red);
    } finally {
      if (file != null) {
        ErrorHandlingFileSystem.deleteIfExists(file);
      }
      _completer?.complete();
    }
  }
}
