// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file

import 'package:flutter_tools/src/base/file_system.dart';

import '../base/common.dart';
import '../base/io.dart';
import '../base/logger.dart';
import '../base/process_manager.dart';
import '../build_info.dart';
import '../cache.dart';
import '../convert.dart';
import '../globals.dart';
import '../project.dart';

import 'windows_configuration.dart';

/// Builds the Windows project through msbuild.
Future<void> buildWindows(WindowsProject windowsProject, BuildInfo buildInfo) async {
  // Update the Generated.props configuration file that contains flutter root
  // and track widget creation inforation.
  updateGeneratedProps(
    windowsProject,
    flutterRoot: Cache.flutterRoot,
    trackWidgetCreation: buildInfo?.trackWidgetCreation == true,
  );

  // Attempt to locate vcvars64.bat.
  final String vcVarsPath = findVcVars();
  if (vcVarsPath == null) {
    printError('Unable to find vcvars64.bat. Proceeding anyway; if it fails,');
    printError('run vcvars64.bat manually in this console then try again.');
  }

  // We need some environment configuration from the vcvars, but we don't want
  // to maintain more batch script logic than necessary.
  final String msBuildScript = '''
call "$vcVarsPath"
msbuild ${windowsProject.solutionFile.path} /p:Configuration=${buildInfo.isDebug ? 'Debug' : 'Release'}
''';
  print(msBuildScript);
  final File msBuildFile = fs.file(fs.path.join(windowsProject.project.directory.path, 'build', 'msbuild.bat'))
    ..createSync(recursive: true)
    ..writeAsStringSync(msBuildScript);

  // Execute msbuild.
  final Process process = await processManager.start(
    <String>[msBuildFile.path],
    runInShell: true,
  );
  final Status status = logger.startProgress(
    'Building Windows application...',
    timeout: null,
  );
  int result;
  try {
    process.stderr
      .transform(utf8.decoder)
      .transform(const LineSplitter())
      .listen(printError);
    process.stdout
      .transform(utf8.decoder)
      .transform(const LineSplitter())
      .listen(printTrace);
    result = await process.exitCode;
  } finally {
    status.cancel();
  }
  if (result != 0) {
    throwToolExit('Build process failed');
  }
}
