// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter_tools/src/context_runner.dart';
import 'package:flutter_tools/src/test/coverage_collector.dart';
import 'package:flutter_tools/src/vmservice.dart';
import 'package:path/path.dart' as path;
import 'package:pedantic/pedantic.dart';
import 'package:coverage/coverage.dart' as coverage;

 /// A temp directory to create synthetic test files in.
final Directory tempDirectory = Directory.systemTemp.createTempSync('_flutter_coverage')
    ..createSync();

 /// A coverage map that can be re-initialized to speed up subsequent runs.
final File coverageMap = File(path.join(Directory.current.path, '.coverage_report', 'coverage_map.json'));

 /// Generates an html coverage report for the flutter_tool.
///
/// By default, this will run all tests under the test directory. For a faster
/// workflow, specific test files to run can also be passed as arguments. In
/// these cases, the existing coverage information is persisted and can be
/// reused.
///
/// Must be run from the flutter_tools directory.
///
/// Requires lcov and genhtml to be on PATH.
/// See: https://github.com/linux-test-project/lcov.git.
Future<void> main(List<String> arguments) async {
  print(arguments);
  final Set<String> testFilter = Set<String>.of(arguments);
  await runInContext(() async {
    final CoverageCollector coverageCollector = ToolCoverageCollector();
    // initialize from existing coverage map if it exists.
    if (coverageMap.existsSync()) {
      try {
        final Map<String, dynamic> pending = json.decode(coverageMap.readAsStringSync());
        final Map<String, Map<int, int>> result = <String, Map<int, int>>{};
        for (String key in pending.keys) {
          final Map<dynamic, dynamic> subMap = pending[key];
          result[key] = Map<dynamic, dynamic>.fromIterables(
            subMap.keys.map<int>((dynamic key) => int.parse(key)),
            subMap.values,
          );
        }
        coverageCollector.globalHitmap = result;
      } catch (_) {
        coverageMap.deleteSync();
      }
    }
    final String flutterRoot = Directory.current.parent.parent.path;
    final String dartPath =  '/Users/jonahwilliams/Documents/engine/src/out/host_debug_unopt/dart';

     for (FileSystemEntity fileSystemEntity in Directory('test').listSync(recursive: true)) {
      if (!fileSystemEntity.path.endsWith('_test.dart')) {
        continue;
      }
      if (testFilter.isNotEmpty && !testFilter.contains(fileSystemEntity.path)) {
        continue;
      }
      final File fakeTest = File(path.join(tempDirectory.path, fileSystemEntity.path))
        ..createSync(recursive: true)
        ..writeAsStringSync('''
import "package:test/test.dart";
import "${path.absolute(fileSystemEntity.path)}" as entrypoint;
 void main() {
  group('', entrypoint.main);
}
''');
      try {
        final int port = await _findPort();
        final Uri coverageUri = Uri.parse('http://127.0.0.1:$port');
        final Process testProcess = await Process.start(dartPath, <String>[
          '--packages=${File('.packages').absolute.path}',
          '--pause-isolates-on-exit',
          '--enable-asserts',
          '--enable-vm-service=${coverageUri.port}',
          fakeTest.path,
        ], runInShell: true, environment: <String, String>{
          'FLUTTER_ROOT': Directory.current.parent.parent.path,
        }).timeout(const Duration(minutes: 1));
        testProcess.stdout
          .transform(utf8.decoder)
          .transform(const LineSplitter())
          .listen(print);
        testProcess.stderr
          .transform(utf8.decoder)
          .transform(const LineSplitter())
          .listen(print);
        unawaited(testProcess.exitCode.then((int code) {
          print('test process exited with $code');
        }));
        await coverageCollector.collectCoverage(testProcess, coverageUri, 'package:flutter_tools').timeout(const Duration(minutes: 1));
      } on TimeoutException catch (err) {
        print('Failed to start ${fakeTest.path}');
        continue;
      }
    }

     if (!coverageMap.existsSync()) {
      coverageMap.createSync(recursive: true);
    }
    print('saving coverage');
    coverageMap.writeAsStringSync(json.encode(coverageCollector.globalHitmap, toEncodable: (dynamic value) {
      if (value is Map) {
        return Map<dynamic, dynamic>.fromIterables(
          value.keys.map<String>((Object key) => key.toString()),
          value.values,
        );
      }
      return value;
    }));
    final String lcovData = await coverageCollector.finalizeCoverage();
    final String lcovPath = path.join('.coverage_report', 'tools.lcov');
    final String htmlPath = path.join('.coverage_report', 'report.html');
    File(lcovPath)
      ..createSync(recursive: true)
      ..writeAsStringSync(lcovData);
    await Process.run('genhtml', <String>[
      lcovPath,
      '-o',
      htmlPath,
    ], runInShell: true);
  });
}

 Future<int> _findPort() async {
  int port = 0;
  ServerSocket serverSocket;
  try {
    serverSocket = await ServerSocket.bind(InternetAddress.loopbackIPv4.address, 0);
    port = serverSocket.port;
  } catch (e) {
    // Failures are signaled by a return value of 0 from this function.
    print('_findPort failed: $e');
  }
  if (serverSocket != null) {
    await serverSocket.close();
  }
  return port;
}

 class ToolCoverageCollector extends CoverageCollector {
  static final String flutterRoot = Directory.current.parent.parent.path;
  static final String pubPath = path.join(flutterRoot, 'bin', 'cache', 'dart-sdk', 'bin', 'pub');

  @override
  Future<void> collectCoverage(Process process, Uri uri, String rootLibUri) async {
    final int pid = process.pid;
    print('pid $pid: collecting coverage data from $uri...');

    Map<String, Map<int, int>> data;
    final Future<void> processComplete = process.exitCode
      .then<void>((int code) {
        throw Exception('Failed to collect coverage, process terminated prematurely with exit code $code.');
      });
    final Future<void> collectionComplete = coverage.collect(uri, true, true, 'package:flutter_tools')
      .then<void>((Map<String, dynamic> result) {
        if (result == null)
          throw Exception('Failed to collect coverage.');
        data = result;
      });
    await Future.any<void>(<Future<void>>[ processComplete, collectionComplete ]);
    print('pid $pid ($uri): collected coverage data; merging...');
    addHitmap(data);
    print('pid $pid ($uri): done merging coverage data into global coverage map.');
  }
}