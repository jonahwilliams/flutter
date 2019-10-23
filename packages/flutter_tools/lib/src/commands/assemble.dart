// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:flutter_tools/src/build_system/depfile.dart';
import 'package:meta/meta.dart';

import '../base/common.dart';
import '../base/file_system.dart';
import '../build_system/build_system.dart';
import '../build_system/targets/assets.dart';
import '../build_system/targets/dart.dart';
import '../build_system/targets/ios.dart';
import '../build_system/targets/linux.dart';
import '../build_system/targets/macos.dart';
import '../build_system/targets/web.dart';
import '../build_system/targets/windows.dart';
import '../globals.dart';
import '../project.dart';
import '../runner/flutter_command.dart';

/// All currently implemented targets.
const List<Target> _kDefaultTargets = <Target>[
  UnpackWindows(),
  CopyAssets(),
  KernelSnapshot(),
  AotElfProfile(),
  AotElfRelease(),
  AotAssemblyProfile(),
  AotAssemblyRelease(),
  DebugMacOSFramework(),
  DebugMacOSBundleFlutterAssets(),
  ProfileMacOSBundleFlutterAssets(),
  ReleaseMacOSBundleFlutterAssets(),
  DebugBundleLinuxAssets(),
  WebReleaseBundle(),
  CopyFlutterBundle(),
  ReleaseCopyFlutterBundle(),
];

/// Assemble provides a low level API to interact with the flutter tool build
/// system.
class AssembleCommand extends FlutterCommand {
  AssembleCommand() {
    argParser.addMultiOption(
      'define',
      abbr: 'd',
      help: 'Allows passing configuration to a target with --define=target=key=value.',
    );
    argParser.addOption('build-inputs', help: 'A file path where a newline '
        'separated file containing all inputs used will be written after a build.'
        ' This file is not included as a build input or output. This file is not'
        ' written if the build fails for any reason.');
    argParser.addOption('build-outputs', help: 'A file path where a newline '
        'separated file containing all outputs used will be written after a build.'
        ' This file is not included as a build input or output. This file is not'
        ' written if the build fails for any reason.');
    argParser.addOption('output', abbr: 'o', help: 'A directory where output '
        'files will be written. Must be either absolute or relative from the '
        'root of the current Flutter project.',
    );
    argParser.addOption('depfile', help: 'A file path where a Make style'
      ' depfile will be written. Must be either absolute or relative from the '
      'root of the current Flutter project.'
    );
    argParser.addOption(
      'resource-pool-size',
      help: 'The maximum number of concurrent tasks the build system will run.',
    );
  }

  @override
  String get description => 'Assemble and build flutter resources.';

  @override
  String get name => 'assemble';

  static Map<String, String> _parseDefines(List<String> values) {
    final Map<String, String> results = <String, String>{};
    for (String chunk in values) {
      final List<String> parts = chunk.split('=');
      if (parts.length != 2) {
        throwToolExit('Improperly formatted define flag: $chunk');
      }
      final String key = parts[0];
      final String value = parts[1];
      results[key] = value;
    }
    return results;
  }

  @override
  Future<FlutterCommandResult> runCommand() async {
    if (argResults.rest.isEmpty) {
      throwToolExit('missing target name for flutter assemble.');
    }
    await AssembleDelegate().build(
      targetName: argResults.rest.first,
      resourcePoolSize: argResults['resourcePoolSize'],
      buildInputs: argResults['build-inputs'],
      buildOutputs: argResults['build-outputs'],
      depfile: argResults['depfile'],
      defines: _parseDefines(argResults['define']),
      output: argResults['output'],
    );
    return null;
  }
}

class AssembleDelegate {
  Future<void> build({
    String targetName,
    String buildInputs,
    String buildOutputs,
    String depfile,
    int resourcePoolSize,
    Map<String, String> defines,
    String output,
  }) async {
    final FlutterProject flutterProject = FlutterProject.current();
    if (output == null) {
      throwToolExit('--output directory is required for assemble.');
    }
    final Target target = _kDefaultTargets
      .firstWhere((Target target) => target.name == targetName, orElse: () => null);
    if (target == null) {
      throwToolExit('No target named "$targetName" defined.');
    }
    // If path is relative, make it absolute from flutter project.
    if (fs.path.isRelative(output)) {
      output = fs.path.join(flutterProject.directory.path, output);
    }
    final Environment environment = Environment(
      outputDir: fs.directory(output),
      buildDir: flutterProject.directory
          .childDirectory('.dart_tool')
          .childDirectory('flutter_build'),
      projectDir: flutterProject.directory,
      defines: defines,
    );
    final BuildResult result = await buildSystem.build(target, environment, buildSystemConfig: BuildSystemConfig(
      resourcePoolSize: resourcePoolSize
    ));
    if (!result.success) {
      for (MapEntry<String, ExceptionMeasurement> data in result.exceptions.entries) {
        printError('Target ${data.key} failed: ${data.value.exception}');
        printError('${data.value.exception}');
      }
      throwToolExit('build failed.');
    }
    printStatus('build succeeded.');
    if (buildInputs != null) {
      writeListIfChanged(result.inputFiles, buildInputs);
    }
    if (buildOutputs != null) {
      writeListIfChanged(result.outputFiles, buildInputs);
    }
    if (depfile != null) {
      final Depfile depfile = Depfile(result.inputFiles, result.outputFiles);
      final File outputFile = fs.file(depfile);
      if (!outputFile.parent.existsSync()) {
        outputFile.parent.createSync(recursive: true);
      }
      depfile.writeToFile(outputFile);
    }
  }
}

@visibleForTesting
void writeListIfChanged(List<File> files, String path) {
  final File file = fs.file(path);
  final StringBuffer buffer = StringBuffer();
  // These files are already sorted.
  for (File file in files) {
    buffer.writeln(file.path);
  }
  final String newContents = buffer.toString();
  if (!file.existsSync()) {
    file.writeAsStringSync(newContents);
  }
  final String currentContents = file.readAsStringSync();
  if (currentContents != newContents) {
    file.writeAsStringSync(newContents);
  }
}
