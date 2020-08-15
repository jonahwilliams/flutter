// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';

import '../commands/build_linux.dart';
import '../commands/build_macos.dart';
import '../commands/build_windows.dart';
import '../globals.dart' as globals;
import '../linux/build_linux.dart';
import '../runner/flutter_command.dart';
import 'build_aar.dart';
import 'build_aot.dart';
import 'build_apk.dart';
import 'build_appbundle.dart';
import 'build_bundle.dart';
import 'build_fuchsia.dart';
import 'build_ios.dart';
import 'build_ios_framework.dart';
import 'build_web.dart';

class BuildCommand extends FlutterCommand {
  BuildCommand({
    bool verboseHelp = false,
    LinuxBuilder linuxBuilder, // TODO(jonahwilliams): make required once command tests do not construct build command directly.
  }) {
    addSubcommand(BuildAarCommand(verboseHelp: verboseHelp));
    addSubcommand(BuildApkCommand(verboseHelp: verboseHelp));
    addSubcommand(BuildAppBundleCommand(verboseHelp: verboseHelp));
    addSubcommand(BuildAotCommand());
    addSubcommand(BuildIOSCommand(verboseHelp: verboseHelp));
    addSubcommand(BuildIOSFrameworkCommand(
      buildSystem: globals.buildSystem,
      verboseHelp: verboseHelp,
    ));
    addSubcommand(BuildBundleCommand(verboseHelp: verboseHelp));
    addSubcommand(BuildWebCommand(verboseHelp: verboseHelp));
    addSubcommand(BuildMacosCommand(verboseHelp: verboseHelp));
    addSubcommand(BuildLinuxCommand(verboseHelp: verboseHelp, linuxBuilder: linuxBuilder));
    addSubcommand(BuildWindowsCommand(verboseHelp: verboseHelp));
    addSubcommand(BuildFuchsiaCommand(verboseHelp: verboseHelp));
  }

  @override
  final String name = 'build';

  @override
  final String description = 'Flutter build commands.';

  @override
  Future<FlutterCommandResult> runCommand() async => null;
}

abstract class BuildSubCommand extends FlutterCommand {
  BuildSubCommand() {
    requiresPubspecYaml();
  }
}
