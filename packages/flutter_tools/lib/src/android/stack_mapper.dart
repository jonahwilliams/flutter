// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

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
class StackMapper extends DelegatingLogger {
  StackMapper({
    required LocalEngineArtifacts artifacts,
    required ProcessManager manager,
    required FileSystem fileSystem,
    required Logger logger,
  }) : _artifacts = artifacts,
       _processManager = manager,
       _fileSystem = fileSystem,
       _logger = logger,
       super(logger);

  final LocalEngineArtifacts _artifacts;
  final ProcessManager _processManager;
  final FileSystem _fileSystem;
  final Logger _logger;

  static const String kCrashHeader = '*** *** *** *** ***'; // repeating a few more times...
  static const String kBackTrace = 'backtrace:';

  // Match a line like `   #06 pc`
  static final RegExp kBackTraceLine = RegExp(r'\w+#[0-9]+ pc');

  StringBuffer? _currentBuffer;
  _MappingState _state = _MappingState.normal;

  @override
  void printStatus(String message, {bool? emphasis, TerminalColor? color, bool? newline, int? indent, int? hangingIndent, bool? wrap}) {
    processLine(message);
    super.printStatus(message, emphasis: emphasis, color: color, newline: newline, indent: indent, hangingIndent: hangingIndent, wrap: wrap);
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
          _state = _MappingState.header;
        }
        break;
      case _MappingState.backtrace:
        if (line.startsWith(kBackTraceLine)) {
          _currentBuffer!.writeln(line);
        } else {
          // The end of the crash has been reached.
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
    final String ndkStackPath = _fileSystem.path.join(engineOutPath, '..', '..', 'third_party', 'android_tools', 'ndk', 'ndk-stack');
    if (!_fileSystem.file(ndkStackPath).existsSync()) {
      _logger.printTrace('Could not find ndk-stack at $ndkStackPath');
    }
    Directory? directory;
    try {
      directory = _fileSystem
        .systemTempDirectory
        .createTempSync('flutter_android_symbols.');
      final File file = directory.childFile('crash.txt');
      file.writeAsStringSync(report);

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
      _logger.printStatus('SYMBOLIZED CRASH', color: TerminalColor.red);
      _logger.printStatus(result.stdout as String);
    } finally {
      if (directory != null) {
        ErrorHandlingFileSystem.deleteIfExists(directory);
      }
    }
  }
}
