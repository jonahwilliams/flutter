// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';
import 'dart:convert';

import 'package:build_daemon/data/build_status.dart';
import 'package:build_daemon/data/build_target.dart';
import 'package:build_daemon/client.dart';
import 'package:meta/meta.dart';

import '../artifacts.dart';
import '../base/context.dart';
import '../base/file_system.dart';
import '../base/io.dart';
import '../base/platform.dart';
import '../base/process_manager.dart';
import '../cache.dart';
import '../compile.dart';
import '../dart/package_map.dart';
import '../globals.dart';
import '../project.dart';

/// The [BuildRunnerFactory] instance.
BuildRunnerFactory get buildRunnerFactory => context[BuildRunnerFactory];

/// Whether to attempt to build a flutter project using build* libraries.
///
/// This requires both an experimental opt in via the environment variable
/// 'FLUTTER_EXPERIMENTAL_BUILD' and that the project itself has a
/// dependency on the package 'flutter_build' and 'build_runner.'
FutureOr<bool> get experimentalBuildEnabled async {
  if (_experimentalBuildEnabled != null) {
    return _experimentalBuildEnabled;
  }
  final bool flagEnabled = platform?.environment['FLUTTER_EXPERIMENTAL_BUILD']?.toLowerCase() == 'true';
  if (!flagEnabled) {
    return _experimentalBuildEnabled = false;
  }
  final FlutterProject flutterProject = await FlutterProject.current();
  final Map<String, Uri> packages = PackageMap(flutterProject.packagesFile.path).map;
  return _experimentalBuildEnabled = packages.containsKey('flutter_build') && packages.containsKey('build_runner');
}
bool _experimentalBuildEnabled;

@visibleForTesting
set experimentalBuildEnabled(bool value) {
  _experimentalBuildEnabled = value;
}

/// An implementation of a [ResidentCompiler] which runs a [BuildRunner] before
/// talking to the frontend_server.
class BuildResidentCompiler implements ResidentCompiler {
  BuildResidentCompiler._(this._residentCompiler, this._buildDaemonClient);

  static Future<BuildResidentCompiler> create({
    @required String mainPath,
    bool trackWidgetCreation = false,
    CompilerMessageConsumer compilerMessageConsumer = printError,
    @required bool unsafePackageSerialization,
  }) async {
      final FlutterProject flutterProject = await FlutterProject.current();
      final BuildRunner buildRunner = buildRunnerFactory.create();
      final BuildDaemonClient buildDaemonClient = await buildRunner.daemon(
        aot: false,
        extraFrontEndOptions: <String>[],
        linkPlatformKernelIn: false,
        mainPath: mainPath,
        targetProductVm: false,
        trackWidgetCreation: null,
        buildTarget: 'flutter',
      );
      final Future<BuildResults> buildResults = buildDaemonClient.buildResults.first;
      buildDaemonClient.startBuild();
      await buildResults;
      final File packagesFile = await buildRunner.packagesFileForEntrypoint(mainPath);
      final ResidentCompiler residentCompiler = ResidentCompiler(
        artifacts.getArtifactPath(Artifact.flutterPatchedSdkPath),
        trackWidgetCreation: trackWidgetCreation,
        packagesPath: packagesFile.path,
        fileSystemRoots: <String>[
          flutterProject.generated.absolute.path,
          flutterProject.directory.path,
        ],
        fileSystemScheme: 'org-dartlang-app',
        targetModel: TargetModel.flutter,
        unsafePackageSerialization: unsafePackageSerialization,
      );
      return BuildResidentCompiler._(residentCompiler, buildDaemonClient);
  }

  final ResidentCompiler _residentCompiler;
  final BuildDaemonClient _buildDaemonClient;

  @override
  void accept() {
    _residentCompiler.accept();
  }

  @override
  Future<CompilerOutput> compileExpression(String expression, List<String> definitions, List<String> typeDefinitions, String libraryUri, String klass, bool isStatic) {
    return _residentCompiler.compileExpression(expression, definitions, typeDefinitions, libraryUri, klass, isStatic);
  }

  @override
  Future<CompilerOutput> recompile(String mainPath, List<String> invalidatedFiles, {String outputPath, String packagesFilePath}) async {
    final Future<BuildResults> pendingBuildResults = _buildDaemonClient.buildResults.first;
    _buildDaemonClient.startBuild();
    await pendingBuildResults;
    if (await fs.file(outputPath).exists()) {
      await fs.file(outputPath).delete();
    }
    return _residentCompiler.recompile(
      mainPath,
      invalidatedFiles,
      outputPath: outputPath,
      packagesFilePath: null, // Intentionally left null,
    );
  }

  @override
  void reject() {
    _residentCompiler.reject();
  }

  @override
  void reset() {
    _residentCompiler.reset();
  }

  @override
  Future<void> shutdown() {
    return _residentCompiler.shutdown();
  }
}

/// An injectable factory to create instances of [BuildRunner].
class BuildRunnerFactory {
  const BuildRunnerFactory();

  /// Creates a new [BuildRunner] instance.
  BuildRunner create() {
    return BuildRunner();
  }
}

/// A wrapper for a build_runner process which delegates to a generated
/// build script.
///
/// This is only enabled if [experimentalBuildEnabled] is true, and only for
/// external flutter users.
class BuildRunner {
  bool get isBusy => _isBusy;
  bool _isBusy = false;

  /// Run a build_runner build and return the resulting .packages and dill file.
  ///
  /// The defines of the build command are the arguments required in the
  /// flutter_build kernel builder.
  Future<FlutterBuildResult> build({
    @required bool aot,
    @required bool linkPlatformKernelIn,
    @required bool trackWidgetCreation,
    @required bool targetProductVm,
    @required String mainPath,
    @required List<String> extraFrontEndOptions,
  }) async {
    if (_isBusy) {
      throw Exception('build_runner is currently running');
    }
    _isBusy = true;
    final FlutterProject flutterProject = await FlutterProject.current();
    final String frontendServerPath = artifacts.getArtifactPath(Artifact.frontendServerSnapshotForEngineDartSdk);
    final String pubExecutable = fs.path.join(Cache.flutterRoot, 'bin', 'cache', 'dart-sdk', 'bin', 'pub');
    final String sdkRoot = artifacts.getArtifactPath(Artifact.flutterPatchedSdkPath);
    final String engineDartBinaryPath = artifacts.getArtifactPath(Artifact.engineDartBinary);
    final String packagesPath = flutterProject.packagesFile.absolute.path;
    final List<String> command = <String>[
      '$pubExecutable',
      'run',
      'build_runner',
      'build',
      '--define',
      'flutter_build|kernel=disabled=false',
      '--define',
      'flutter_build|kernel=aot=$aot',
      '--define',
      'flutter_build|kernel=linkPlatformKernelIn=$linkPlatformKernelIn',
      '--define',
      'flutter_build|kernel=trackWidgetCreation=${trackWidgetCreation ?? false}',
      '--define',
      'flutter_build|kernel=targetProductVm=$targetProductVm',
      '--define',
      'flutter_build|kernel=mainPath=$mainPath',
      '--define',
      'flutter_build|kernel=packagesPath=$packagesPath',
      '--define',
      'flutter_build|kernel=sdkRoot=$sdkRoot',
      '--define',
      'flutter_build|kernel=frontendServerPath=$frontendServerPath',
      '--define',
      'flutter_build|kernel=engineDartBinaryPath=$engineDartBinaryPath',
      '--define',
      'flutter_build|kernel=extraFrontEndOptions=${extraFrontEndOptions ?? const <String>[]}',
      '--delete-conflicting-outputs',
    ];
    printTrace(command.toString());
    final Process process = await processManager.start(command);
    process.stdout
      .transform(utf8.decoder)
      .transform(const LineSplitter())
      .listen(_handleOutput);
    process.stderr
      .transform(utf8.decoder)
      .transform(const LineSplitter())
      .listen(_handleError);
    final int exitCode = await process.exitCode;
    if (exitCode != 0) {
      _isBusy = false;
      throw Exception('build_runner exited with non-zero exit code: $exitCode');
    }

    /// We don't check for this above because it might be generated for the
    /// first time by invoking the build.
    final Directory generatedDirectory = flutterProject.generated;
    if (!await generatedDirectory.exists()) {
      _isBusy = false;
      throw Exception('build_runner cannot find generated directory');
    }
    final File packagesFile = await packagesFileForEntrypoint(mainPath);
    final File dillFile = await dillFileForEntrypoint(mainPath);
    if (!await packagesFile.exists() || !await dillFile.exists()) {
      _isBusy = false;
      throw Exception('build_runner did not produce output at expected location: ${dillFile.path} missing');
    }
    _isBusy = false;
    return FlutterBuildResult(packagesFile, dillFile);
  }

  Future<BuildDaemonClient> daemon({
    @required bool aot,
    @required bool linkPlatformKernelIn,
    @required bool trackWidgetCreation,
    @required bool targetProductVm,
    @required String mainPath,
    @required List<String> extraFrontEndOptions,
    @required String buildTarget,
  }) async {
    final FlutterProject flutterProject = await FlutterProject.current();
    final String frontendServerPath = artifacts.getArtifactPath(Artifact.frontendServerSnapshotForEngineDartSdk);
    final String pubExecutable = fs.path.join(Cache.flutterRoot, 'bin', 'cache', 'dart-sdk', 'bin', 'pub');
    final String sdkRoot = artifacts.getArtifactPath(Artifact.flutterPatchedSdkPath);
    final String engineDartBinaryPath = artifacts.getArtifactPath(Artifact.engineDartBinary);
    final String packagesPath = flutterProject.packagesFile.absolute.path;
    final List<String> command = <String>[
      '$pubExecutable',
      'run',
      'build_runner',
      'daemon',
      '--define',
      'flutter_build|kernel=disabled=false',
      '--define',
      'flutter_build|kernel=aot=$aot',
      '--define',
      'flutter_build|kernel=linkPlatformKernelIn=$linkPlatformKernelIn',
      '--define',
      'flutter_build|kernel=trackWidgetCreation=${trackWidgetCreation ?? false}',
      '--define',
      'flutter_build|kernel=targetProductVm=$targetProductVm',
      '--define',
      'flutter_build|kernel=mainPath=$mainPath',
      '--define',
      'flutter_build|kernel=packagesPath=$packagesPath',
      '--define',
      'flutter_build|kernel=sdkRoot=$sdkRoot',
      '--define',
      'flutter_build|kernel=frontendServerPath=$frontendServerPath',
      '--define',
      'flutter_build|kernel=engineDartBinaryPath=$engineDartBinaryPath',
      '--define',
      'flutter_build|kernel=extraFrontEndOptions=${extraFrontEndOptions ?? const <String>[]}',
      '--delete-conflicting-outputs'
    ];
    final BuildDaemonClient client = await BuildDaemonClient.connect(flutterProject.directory.path, command);
    client.registerBuildTarget(DefaultBuildTarget((DefaultBuildTargetBuilder builder) {
      builder..target = buildTarget;
    }));
    return client;
  }

  Future<File> packagesFileForEntrypoint(String mainPath) async {
    final FlutterProject flutterProject = await FlutterProject.current();
    final String relativeMain = fs.path.relative(mainPath, from: flutterProject.directory.path);
    return fs.file(fs.path.join(flutterProject.generated.path, fs.path.setExtension(relativeMain, '.packages')));
  }

  Future<File> dillFileForEntrypoint(String mainPath) async {
    final FlutterProject flutterProject = await FlutterProject.current();
    final String relativeMain = fs.path.relative(mainPath, from: flutterProject.directory.path);
    return fs.file(fs.path.join(flutterProject.generated.path, fs.path.setExtension(relativeMain, '.app.dill')));
  }

  void _handleOutput(String line) {
    printTrace(line);
  }

  void _handleError(String line) {
    printError(line);
  }
}

class FlutterBuildResult {
  const FlutterBuildResult(this.packagesFile, this.dillFile);

  final File packagesFile;
  final File dillFile;
}
