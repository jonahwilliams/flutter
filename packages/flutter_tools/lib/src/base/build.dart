// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';

import 'package:meta/meta.dart';

import '../artifacts.dart';
import '../base/logger.dart';
import '../build_info.dart';
import '../bundle.dart';
import '../compile.dart';
import '../macos/xcode.dart';
import '../project.dart';
import '../reporting/reporting.dart';
import 'file_system.dart';
import 'process.dart';

/// A snapshot build configuration.
class SnapshotType {
  SnapshotType(this.platform, this.mode)
    : assert(mode != null);

  final TargetPlatform platform;
  final BuildMode mode;

  @override
  String toString() => '$platform $mode';
}

/// Interface to the gen_snapshot command-line tool.
class GenSnapshot {
  const GenSnapshot({
    @required Artifacts artifacts,
  }) : _artifacts = artifacts;

  final Artifacts _artifacts;

  String getSnapshotterPath(SnapshotType snapshotType) {
    return _artifacts.getArtifactPath(
        Artifact.genSnapshot, platform: snapshotType.platform, mode: snapshotType.mode);
  }

  Future<int> run({
    @required SnapshotType snapshotType,
    DarwinArch darwinArch,
    Iterable<String> additionalArgs = const <String>[],
  }) {
    final List<String> args = <String>[
      ...additionalArgs,
    ];

    String snapshotterPath = getSnapshotterPath(snapshotType);

    // iOS has a separate gen_snapshot for armv7 and arm64 in the same,
    // directory. So we need to select the right one.
    if (snapshotType.platform == TargetPlatform.ios) {
      snapshotterPath += '_' + getNameForDarwinArch(darwinArch);
    }

    StringConverter outputFilter;
    if (additionalArgs.contains('--strip')) {
      // Filter out gen_snapshot's warning message about stripping debug symbols
      // from ELF library snapshots.
      const String kStripWarning = 'Warning: Generating ELF library without DWARF debugging information.';
      const String kAssemblyStripWarning = 'Warning: Generating assembly code without DWARF debugging information.';
      outputFilter = (String line) => line != kStripWarning && line != kAssemblyStripWarning ? line : null;
    }

    return processUtils.stream(
      <String>[snapshotterPath, ...args],
      mapFunction: outputFilter,
    );
  }
}

class AOTSnapshotter {
  AOTSnapshotter({
    this.reportTimings = false,
    @required Logger logger,
    @required FileSystem fileSystem,
    @required Artifacts artifacts,
    @required Xcode xcode,
    @required GenSnapshot genSnapshot,
  }) : _logger = logger,
       _fileSystem = fileSystem,
       _artifacts = artifacts,
       _xcode = xcode,
       _genSnapshot = genSnapshot;

  final Logger _logger;
  final FileSystem _fileSystem;
  final Artifacts _artifacts;
  final Xcode _xcode;
  final GenSnapshot _genSnapshot;

  /// If true then AOTSnapshotter would report timings for individual building
  /// steps (Dart front-end parsing and snapshot generation) in a stable
  /// machine readable form. See [AOTSnapshotter._timedStep].
  final bool reportTimings;

  /// Builds an architecture-specific ahead-of-time compiled snapshot of the specified script.
  Future<int> build({
    @required TargetPlatform platform,
    @required BuildMode buildMode,
    @required String mainPath,
    @required String packagesPath,
    @required String outputPath,
    DarwinArch darwinArch,
    List<String> extraGenSnapshotOptions = const <String>[],
    @required bool bitcode,
    @required String splitDebugInfo,
    bool quiet = false,
  }) async {
    if (bitcode && platform != TargetPlatform.ios) {
      _logger.printError('Bitcode is only supported for iOS.');
      return 1;
    }

    if (!_isValidAotPlatform(platform, buildMode)) {
      _logger.printError('${getNameForTargetPlatform(platform)} does not support AOT compilation.');
      return 1;
    }
    // TODO(cbracken): replace IOSArch with TargetPlatform.ios_{armv7,arm64}.
    assert(platform != TargetPlatform.ios || darwinArch != null);

    final Directory outputDir = _fileSystem.directory(outputPath);
    outputDir.createSync(recursive: true);

    final List<String> genSnapshotArgs = <String>[
      '--deterministic',
    ];
    if (extraGenSnapshotOptions != null && extraGenSnapshotOptions.isNotEmpty) {
      _logger.printTrace('Extra gen_snapshot options: $extraGenSnapshotOptions');
      genSnapshotArgs.addAll(extraGenSnapshotOptions);
    }

    final String assembly = _fileSystem.path.join(outputDir.path, 'snapshot_assembly.S');
    if (platform == TargetPlatform.ios || platform == TargetPlatform.darwin_x64) {
      // Assembly AOT snapshot.
      genSnapshotArgs.add('--snapshot_kind=app-aot-assembly');
      genSnapshotArgs.add('--assembly=$assembly');
      genSnapshotArgs.add('--strip');
    } else {
      final String aotSharedLibrary = _fileSystem.path.join(outputDir.path, 'app.so');
      genSnapshotArgs.add('--snapshot_kind=app-aot-elf');
      genSnapshotArgs.add('--elf=$aotSharedLibrary');
      genSnapshotArgs.add('--strip');
    }

    if (platform == TargetPlatform.android_arm || darwinArch == DarwinArch.armv7) {
      // Use softfp for Android armv7 devices.
      // This is the default for armv7 iOS builds, but harmless to set.
      // TODO(cbracken): eliminate this when we fix https://github.com/flutter/flutter/issues/17489
      genSnapshotArgs.add('--no-sim-use-hardfp');

      // Not supported by the Pixel in 32-bit mode.
      genSnapshotArgs.add('--no-use-integer-division');
    }

    // The name of the debug file must contain additonal information about
    // the architecture, since a single build command may produce
    // multiple debug files.
    final String archName = getNameForTargetPlatform(platform, darwinArch: darwinArch);
    final String debugFilename = 'app.$archName.symbols';
    if (splitDebugInfo?.isNotEmpty ?? false) {
      _fileSystem.directory(splitDebugInfo)
        .createSync(recursive: true);
    }

    // Optimization arguments.
    genSnapshotArgs.addAll(<String>[
      // Faster async/await
      '--no-causal-async-stacks',
      '--lazy-async-stacks',
      if (splitDebugInfo?.isNotEmpty ?? false) ...<String>[
        '--dwarf-stack-traces',
        '--save-debugging-info=${_fileSystem.path.join(splitDebugInfo, debugFilename)}'
      ]
    ]);

    genSnapshotArgs.add(mainPath);

    final SnapshotType snapshotType = SnapshotType(platform, buildMode);
    final int genSnapshotExitCode =
      await _timedStep('snapshot(CompileTime)', 'aot-snapshot',
        () => _genSnapshot.run(
      snapshotType: snapshotType,
      additionalArgs: genSnapshotArgs,
      darwinArch: darwinArch,
    ));
    if (genSnapshotExitCode != 0) {
      _logger.printError('Dart snapshot generator failed with exit code $genSnapshotExitCode');
      return genSnapshotExitCode;
    }

    // On iOS and macOS, we use Xcode to compile the snapshot into a dynamic library that the
    // end-developer can link into their app.
    if (platform == TargetPlatform.ios || platform == TargetPlatform.darwin_x64) {
      final RunResult result = await _buildFramework(
        appleArch: darwinArch,
        isIOS: platform == TargetPlatform.ios,
        assemblyPath: assembly,
        outputPath: outputDir.path,
        bitcode: bitcode,
        quiet: quiet,
      );
      if (result.exitCode != 0) {
        return result.exitCode;
      }
    }
    return 0;
  }

  /// Builds an iOS or macOS framework at [outputPath]/App.framework from the assembly
  /// source at [assemblyPath].
  Future<RunResult> _buildFramework({
    @required DarwinArch appleArch,
    @required bool isIOS,
    @required String assemblyPath,
    @required String outputPath,
    @required bool bitcode,
    @required bool quiet
  }) async {
    final String targetArch = getNameForDarwinArch(appleArch);
    if (!quiet) {
      _logger.printStatus('Building App.framework for $targetArch...');
    }

    final List<String> commonBuildOptions = <String>[
      '-arch', targetArch,
      if (isIOS)
        '-miphoneos-version-min=8.0',
    ];

    const String embedBitcodeArg = '-fembed-bitcode';
    final String assemblyO = _fileSystem.path.join(outputPath, 'snapshot_assembly.o');
    List<String> isysrootArgs;
    if (isIOS) {
      final String iPhoneSDKLocation = await _xcode.sdkLocation(SdkType.iPhone);
      if (iPhoneSDKLocation != null) {
        isysrootArgs = <String>['-isysroot', iPhoneSDKLocation];
      }
    }
    final RunResult compileResult = await _xcode.cc(<String>[
      '-arch', targetArch,
      if (isysrootArgs != null) ...isysrootArgs,
      if (bitcode) embedBitcodeArg,
      '-c',
      assemblyPath,
      '-o',
      assemblyO,
    ]);
    if (compileResult.exitCode != 0) {
      _logger.printError('Failed to compile AOT snapshot. Compiler terminated with exit code ${compileResult.exitCode}');
      return compileResult;
    }

    final String frameworkDir = _fileSystem.path.join(outputPath, 'App.framework');
    _fileSystem.directory(frameworkDir).createSync(recursive: true);
    final String appLib = _fileSystem.path.join(frameworkDir, 'App');
    final List<String> linkArgs = <String>[
      ...commonBuildOptions,
      '-dynamiclib',
      '-Xlinker', '-rpath', '-Xlinker', '@executable_path/Frameworks',
      '-Xlinker', '-rpath', '-Xlinker', '@loader_path/Frameworks',
      '-install_name', '@rpath/App.framework/App',
      if (bitcode) embedBitcodeArg,
      if (isysrootArgs != null) ...isysrootArgs,
      '-o', appLib,
      assemblyO,
    ];
    final RunResult linkResult = await _xcode.clang(linkArgs);
    if (linkResult.exitCode != 0) {
      _logger.printError('Failed to link AOT snapshot. Linker terminated with exit code ${compileResult.exitCode}');
    }
    return linkResult;
  }

  /// Compiles a Dart file to kernel.
  ///
  /// Returns the output kernel file path, or null on failure.
  Future<String> compileKernel({
    @required TargetPlatform platform,
    @required BuildMode buildMode,
    @required String mainPath,
    @required String packagesPath,
    @required String outputPath,
    @required bool trackWidgetCreation,
    @required List<String> dartDefines,
    List<String> extraFrontEndOptions = const <String>[],
  }) async {
    final FlutterProject flutterProject = FlutterProject.current();
    final Directory outputDir = _fileSystem.directory(outputPath);
    outputDir.createSync(recursive: true);

    _logger.printTrace('Compiling Dart to kernel: $mainPath');

    if ((extraFrontEndOptions != null) && extraFrontEndOptions.isNotEmpty) {
      _logger.printTrace('Extra front-end options: $extraFrontEndOptions');
    }

    final String depfilePath = _fileSystem.path.join(outputPath, 'kernel_compile.d');
    final KernelCompiler kernelCompiler = await kernelCompilerFactory.create(flutterProject);
    final CompilerOutput compilerOutput =
      await _timedStep('frontend(CompileTime)', 'aot-kernel',
        () => kernelCompiler.compile(
      sdkRoot: _artifacts.getArtifactPath(Artifact.flutterPatchedSdkPath, mode: buildMode),
      mainPath: mainPath,
      packagesPath: packagesPath,
      outputFilePath: getKernelPathForTransformerOptions(
        _fileSystem.path.join(outputPath, 'app.dill'),
        trackWidgetCreation: trackWidgetCreation,
      ),
      depFilePath: depfilePath,
      extraFrontEndOptions: extraFrontEndOptions,
      linkPlatformKernelIn: true,
      aot: true,
      buildMode: buildMode,
      trackWidgetCreation: trackWidgetCreation,
      dartDefines: dartDefines,
    ));

    // Write path to frontend_server, since things need to be re-generated when that changes.
    final String frontendPath = _artifacts.getArtifactPath(Artifact.frontendServerSnapshotForEngineDartSdk);
    _fileSystem.directory(outputPath).childFile('frontend_server.d').writeAsStringSync('frontend_server.d: $frontendPath\n');

    return compilerOutput?.outputFilename;
  }

  bool _isValidAotPlatform(TargetPlatform platform, BuildMode buildMode) {
    if (buildMode == BuildMode.debug) {
      return false;
    }
    return const <TargetPlatform>[
      TargetPlatform.android_arm,
      TargetPlatform.android_arm64,
      TargetPlatform.android_x64,
      TargetPlatform.ios,
      TargetPlatform.darwin_x64,
    ].contains(platform);
  }

  /// This method is used to measure duration of an action and emit it into
  /// verbose output from flutter_tool for other tools (e.g. benchmark runner)
  /// to find.
  /// Important: external performance tracking tools expect format of this
  /// output to be stable.
  Future<T> _timedStep<T>(String marker, String analyticsVar, FutureOr<T> Function() action) async {
    final Stopwatch sw = Stopwatch()..start();
    final T value = await action();
    if (reportTimings) {
      _logger.printStatus('$marker: ${sw.elapsedMilliseconds} ms.');
    }
    flutterUsage.sendTiming('build', analyticsVar, Duration(milliseconds: sw.elapsedMilliseconds));
    return value;
  }
}
