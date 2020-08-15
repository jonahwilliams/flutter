// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:meta/meta.dart';
import 'package:process/process.dart';

import '../artifacts.dart';
import '../base/common.dart';
import '../base/file_system.dart';
import '../base/logger.dart';
import '../base/process.dart';
import '../base/utils.dart';
import '../build_info.dart';
import '../cache.dart';
import '../cmake.dart';
import '../plugins.dart';
import '../project.dart';
import '../reporting/reporting.dart';

class LinuxBuilder {
  LinuxBuilder({
    @required FileSystem fileSystem,
    @required Logger logger,
    @required Artifacts artifacts,
    @required Usage usage,
    @required ProcessManager processManager,
  }) : _fileSystem = fileSystem,
       _logger = logger,
       _artifacts = artifacts,
       _usage = usage,
       _processUtils = ProcessUtils(
         logger: logger,
         processManager: processManager,
       );

  final FileSystem _fileSystem;
  final Logger _logger;
  final Artifacts _artifacts;
  final Usage _usage;
  final ProcessUtils _processUtils;

  /// Builds the Linux project through the Makefile.
  Future<void> buildLinux(
    LinuxProject linuxProject,
    BuildInfo buildInfo, {
    String target = 'lib/main.dart',
  }) async {
    if (!linuxProject.cmakeFile.existsSync()) {
      throwToolExit('No Linux desktop project configured. See '
        'https://github.com/flutter/flutter/wiki/Desktop-shells#create '
        'to learn about adding Linux support to a project.');
    }

    // Build the environment that needs to be set for the re-entrant flutter build
    // step.
    final Map<String, String> environmentConfig = buildInfo.toEnvironmentConfig();
    environmentConfig['FLUTTER_TARGET'] = target;
    if (_artifacts is LocalEngineArtifacts) {
      final String engineOutPath = (_artifacts as LocalEngineArtifacts).engineOutPath;
      environmentConfig['FLUTTER_ENGINE'] = _fileSystem.path.dirname(_fileSystem.path.dirname(engineOutPath));
      environmentConfig['LOCAL_ENGINE'] = _fileSystem.path.basename(engineOutPath);
    }
    writeGeneratedCmakeConfig(Cache.flutterRoot, linuxProject, environmentConfig);

    createPluginSymlinks(linuxProject.parent);

    final Status status = _logger.startProgress(
      'Building Linux application...',
      timeout: null,
    );
    try {
      final String buildModeName = getNameForBuildMode(buildInfo.mode ?? BuildMode.release);
      final Directory buildDirectory = _fileSystem.directory(getLinuxBuildDirectory()).childDirectory(buildModeName);
      await _runCmake(buildModeName, linuxProject.cmakeFile.parent, buildDirectory);
      await _runBuild(buildDirectory);
    } finally {
      status.cancel();
    }
  }

  Future<void> _runCmake(
    String buildModeName,
    Directory sourceDir,
    Directory buildDir,
  ) async {
    final Stopwatch sw = Stopwatch()..start();

    await buildDir.create(recursive: true);

    final String buildFlag = toTitleCase(buildModeName);
    int result;
    try {
      result = await _processUtils.stream(
        <String>[
          'cmake',
          '-G',
          'Ninja',
          '-DCMAKE_BUILD_TYPE=$buildFlag',
          sourceDir.path,
        ],
        workingDirectory: buildDir.path,
        environment: <String, String>{
          'CC': 'clang',
          'CXX': 'clang++'
        },
        trace: true,
      );
    } on ArgumentError {
      throwToolExit("cmake not found. Run 'flutter doctor' for more information.");
    }
    if (result != 0) {
      throwToolExit('Unable to generate build files');
    }
    _usage.sendTiming('build', 'cmake-linux', Duration(milliseconds: sw.elapsedMilliseconds));
  }

  Future<void> _runBuild(Directory buildDir) async {
    final Stopwatch sw = Stopwatch()..start();

    int result;
    try {
      result = await processUtils.stream(
        <String>[
          'ninja',
          '-C',
          buildDir.path,
          'install',
        ],
        environment: <String, String>{
          if (_logger.isVerbose)
            'VERBOSE_SCRIPT_LOGGING': 'true'
        },
        trace: true,
      );
    } on ArgumentError {
      throwToolExit("ninja not found. Run 'flutter doctor' for more information.");
    }
    if (result != 0) {
      throwToolExit('Build process failed');
    }
    _usage.sendTiming('build', 'linux-ninja', Duration(milliseconds: sw.elapsedMilliseconds));
  }
}
