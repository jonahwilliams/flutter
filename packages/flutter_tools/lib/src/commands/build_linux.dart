// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';

import 'package:meta/meta.dart';

import '../base/common.dart';
import '../build_info.dart';
import '../cache.dart';
import '../features.dart';
import '../globals.dart' as globals;
import '../linux/build_linux.dart';
import '../project.dart';
import '../runner/flutter_command.dart' show FlutterCommandResult;
import 'build.dart';

/// A command to build a linux desktop target through a build shell script.
class BuildLinuxCommand extends BuildSubCommand {
  BuildLinuxCommand({
    bool verboseHelp = false,
    @required LinuxBuilder linuxBuilder,
  }) : _linuxBuilder = linuxBuilder {
    addTreeShakeIconsFlag();
    usesTargetOption();
    addBuildModeFlags(verboseHelp: verboseHelp);
    usesPubOption();
    addSplitDebugInfoOption();
    addDartObfuscationOption();
    usesDartDefineOption();
    usesExtraFrontendOptions();
    addEnableExperimentation(hide: !verboseHelp);
    usesTrackWidgetCreation(verboseHelp: verboseHelp);
    addBuildPerformanceFile(hide: !verboseHelp);
    addBundleSkSLPathOption(hide: !verboseHelp);
    addNullSafetyModeOptions(hide: !verboseHelp);
  }

  @override
  final String name = 'linux';
  final LinuxBuilder _linuxBuilder;

  @override
  bool get hidden => !featureFlags.isLinuxEnabled || !globals.platform.isLinux;

  @override
  Future<Set<DevelopmentArtifact>> get requiredArtifacts async => <DevelopmentArtifact>{
    DevelopmentArtifact.linux,
  };

  @override
  String get description => 'Build a Linux desktop application.';

  @override
  Future<FlutterCommandResult> runCommand() async {
    final BuildInfo buildInfo = getBuildInfo();
    final FlutterProject flutterProject = FlutterProject.current();
    if (!featureFlags.isLinuxEnabled) {
      throwToolExit('"build linux" is not currently supported.');
    }
    if (!globals.platform.isLinux) {
      throwToolExit('"build linux" only supported on Linux hosts.');
    }
    await _linuxBuilder.buildLinux(
      flutterProject.linux,
      buildInfo,
      target: targetFile,
    );
    return FlutterCommandResult.success();
  }
}
