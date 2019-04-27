// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:convert';
import 'dart:io';

import 'package:flutter_tools_api/flutter_tools_api.dart';
import 'package:path/path.dart' as path;

/// A macOS device represents a desktop target on the macOS operating system.
class MacOSDevice implements Device {
  const MacOSDevice();

  @override
  Future<Uri> launch(Session session) async {
    return null;
  }

  @override
  Future<void> stop(Session session) async {
    final Process process = session.retrieve<Process>();
    process.kill();
  }

  @override
  Future<DeviceConfiguration> get deviceConfiguration async {
    return DeviceConfiguration(
      <PerformanceMode>{PerformanceMode.debug, PerformanceMode.release},
      <CompileMode>{CompileMode.jit},
    );
  }

  @override
  Future<TargetPlatform> get targetPlatform async => TargetPlatform.darwin_x64;

  @override
  String get id => 'localhost';
}

class MacOSWorkflow implements Workflow {
  const MacOSWorkflow();

  /// Contains definitions for FLUTTER_ROOT, LOCAL_ENGINE.
  static final String generatedXcodePropertiesPath = path.join('Flutter', 'Generated.xcconfig');

  /// The Xcode project file.
  static const String xcodeProjectPath = 'Runner.xcodeproj';

  /// The SYMROOT override directory.
  static final String symrootPath = path.join('Build', 'Products');

  @override
  Future<void> build(BuildRequest buildRequest) async {
    final String symroot = path.join(buildRequest.cacheDirectory.path, symrootPath);
    final String config = buildRequest.performanceMode == PerformanceMode.release ? 'Release' : 'Debug';
    final String objRoot = path.join(buildRequest.cacheDirectory.absolute.path, 'Build', 'Intermediates.noindex');
    final Process process = await Process.start('/usr/bin/env', <String>[
      'xcrun',
      'xcodebuild',
      '-project', xcodeProjectPath,
      '-configuration', config,
      '-scheme', 'Runner',
      '-derivedDataPath', buildRequest.cacheDirectory.absolute.path,
      'OBJROOT=$objRoot',
      'SYMROOT=$symroot',
    ], runInShell: true);
    process.stderr
      .transform(utf8.decoder)
      .transform(const LineSplitter())
      .listen(print);
    process.stdout
      .transform(utf8.decoder)
      .transform(const LineSplitter())
      .listen(print);
    await process.exitCode;
  }
}

class MacOSExtension implements Extension {
  const MacOSExtension();

  @override
  Future<List<Device>> listDevices() async {
    return const <Device>[MacOSDevice()];
  }

  @override
  Workflow get workflow => const MacOSWorkflow();

  @override
  // TODO: implement doctor
  Doctor get doctor => null;
}
