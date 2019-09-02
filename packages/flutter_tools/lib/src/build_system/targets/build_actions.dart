// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';
import 'dart:io' as io;

import 'package:pool/pool.dart';

import '../../artifacts.dart';
import '../../asset.dart';
import '../../base/build.dart';
import '../../base/context.dart';
import '../../base/file_system.dart';
import '../../base/process.dart';
import '../../build_info.dart';
import '../../compile.dart';
import '../../devfs.dart';
import '../../globals.dart';
import '../../macos/xcode.dart';
import '../../project.dart';
import '../build_system.dart';
import 'dart.dart';

/// The type signature for a dart build action.
typedef BuildAction = FutureOr<void> Function(Environment);

/// The default [BuildActionRegistry] instance.
const BuildActionRegistry _defaultRegistry = BuildActionRegistry(actions: <String, BuildAction>{
  'kernel_snapshot': kernelSnapshot,
  'macos_assets': macosAssets,
  'macos_bundle': macosBundle,
});

/// The [BuildActionRegistry] instance.
///
/// By default this isn't injected and a default instance is used instead. This
/// default instance is not capable of registering new targets.
BuildActionRegistry get buildActionRegistry => context.get<BuildActionRegistry>() ?? _defaultRegistry;

/// Registers rule implementations that are looked up by the [RuleParser].
class BuildActionRegistry {
  const BuildActionRegistry({
    Map<String, BuildAction> actions = const <String, BuildAction>{},
  }) : _actions = actions;

  final Map<String, BuildAction> _actions;

  /// Register a [BuildAction] under [name].
  ///
  /// Throws a [StateError] If another action is already registered to that
  /// name.
  void register(String name, BuildAction buildAction) {
    if (_actions.containsKey(name)) {
      throw StateError('$name is already registered.');
    }
    _actions[name] = buildAction;
  }

  /// Resolve [name] to a [BuildAction] instance.
  ///
  /// Throws a [StateError] If no action is registered with that name.
  BuildAction resolve(String name) {
    if (!_actions.containsKey(name)) {
      throw StateError('$name is not registered.');
    }
    return _actions[name];
  }
}

/// The special implementation of copy.
FutureOr<void> copy(Environment environment, List<File> inputs, List<File> outputs) async {
  for (int i = 0; i < inputs.length; i++) {
    final String input = inputs[i].path;
    final String output = outputs[i].path;
    final FileSystemEntityType type = io.FileSystemEntity.typeSync(input);
    switch (type) {
      case FileSystemEntityType.directory:
      case FileSystemEntityType.notFound:
        continue;
      case FileSystemEntityType.file:
        fs.file(input).copySync(output);
        break;
      case FileSystemEntityType.link:
        final String linkPath = fs.link(input).resolveSymbolicLinksSync();
        final Link newLink = fs.link(output);
        newLink.createSync(linkPath);
    }
  }
}

/// Complies a dart project to a kernel dill.
FutureOr<void> kernelSnapshot(Environment environment) async {
  final KernelCompiler compiler = await kernelCompilerFactory.create(
    FlutterProject.fromDirectory(environment.projectDir),
  );
  final BuildMode buildMode = getBuildModeForName(environment.defines[kBuildMode]);
  final String targetFile = environment.defines[kTargetFile] ?? fs.path.join('lib', 'main.dart');
  final String packagesPath = environment.projectDir.childFile('.packages').path;
  final PackageUriMapper packageUriMapper = PackageUriMapper(targetFile,
      packagesPath, null, null);

  final CompilerOutput output = await compiler.compile(
    sdkRoot: artifacts.getArtifactPath(Artifact.flutterPatchedSdkPath, mode: buildMode),
    aot: buildMode != BuildMode.debug,
    trackWidgetCreation: buildMode == BuildMode.debug,
    targetModel: TargetModel.flutter,
    targetProductVm: buildMode == BuildMode.release,
    outputFilePath: environment.buildDir.childFile('app.dill').path,
    depFilePath: null,
    packagesPath: packagesPath,
    linkPlatformKernelIn: buildMode == BuildMode.release,
    mainPath: packageUriMapper.map(targetFile)?.toString() ?? targetFile,
  );
  if (output.errorCount != 0) {
    throw Exception('Errors during snapshot creation: $output');
  }
}

/// Parses the asset manifest (pubspec.yaml) and places assets into the
/// framework.
FutureOr<void> macosAssets(Environment environment) async {
  final Directory frameworkRootDirectory = environment
      .outputDir
      .childDirectory('App.framework');
  final Directory outputDirectory = frameworkRootDirectory
      .childDirectory('Versions')
      .childDirectory('A')
      .childDirectory('Resources')
      .childDirectory('flutter_assets')
      ..createSync(recursive: true);
  /// Copy font manifest, asset manifest, and LICENSE file.
  // TODO(jonahwilliams): move to separate rule once asset bundling is further
  // integrated into the build system.
  final AssetBundle bundle = AssetBundleFactory.instance.createBundle();
  await bundle.build();
    try {
      final Pool pool = Pool(64);
      await Future.wait<void>(
        bundle.entries.entries.map<Future<void>>((MapEntry<String, DevFSContent> entry) async {
          final PoolResource resource = await pool.request();
          try {
            final File file = fs.file(fs.path.join(outputDirectory.path, entry.key));
            file.parent.createSync(recursive: true);
            final DevFSContent content = entry.value;
            // avoid roundtrip through dart heap if real file.
            if (content is DevFSFileContent) {
              await (content.file as File).copy(file.path);
            } else {
              await file.writeAsBytes(await entry.value.contentsAsBytes());
            }
          } finally {
            resource.release();
          }
        }));
    } catch (err, st) {
      throw Exception('Failed to copy assets: $st');
    }
}

FutureOr<void> compileMacosFramework(Environment environment) async {
  final BuildMode buildMode = getBuildModeForName(environment.defines[kBuildMode]);
  if (buildMode == BuildMode.debug) {
    final File outputFile = fs.file(fs.path.join(
      environment.buildDir.path, 'App.framework', 'App'));
    outputFile.createSync(recursive: true);
    final File debugApp = environment.buildDir.childFile('debug_app.cc')
      ..writeAsStringSync(r'''static const int Moo = 88;\n''');
    final RunResult result = await xcode.clang(<String>[
      '-x',
      'c',
      debugApp.path,
      '-arch', 'x86_64',
      '-dynamiclib',
      '-Xlinker', '-rpath', '-Xlinker', '@executable_path/Frameworks',
      '-Xlinker', '-rpath', '-Xlinker', '@loader_path/Frameworks',
      '-install_name', '@rpath/App.framework/App',
      '-o', outputFile.path,
    ]);
    if (result.exitCode != 0) {
      throw Exception('Failed to compile debug App.framework');
    }
    return;
  }
  final int result = await AOTSnapshotter(reportTimings: false).build(
    bitcode: false,
    buildMode: buildMode,
    mainPath: environment.buildDir.childFile('app.dill').path,
    outputPath: environment.buildDir.path,
    platform: TargetPlatform.darwin_x64,
    darwinArch: DarwinArch.x86_64,
    packagesPath: environment.projectDir.childFile('.packages').path,
  );
  if (result != 0) {
    throw Exception('gen shapshot failed.');
  }
}

/// Creates a macOS framework.
///
/// In debug mode, also include the app.dill and precompiled runtimes.
///
/// See https://developer.apple.com/library/archive/documentation/MacOSX/Conceptual/BPFrameworks/Concepts/FrameworkAnatomy.html
/// for more information on Framework structure.
FutureOr<void> macosBundle(Environment environment) async {
  final BuildMode buildMode = getBuildModeForName(environment.defines[kBuildMode]);
  final Directory frameworkRootDirectory = environment
      .outputDir
      .childDirectory('App.framework');
  final Directory outputDirectory = frameworkRootDirectory
      .childDirectory('Versions')
      .childDirectory('A')
      ..createSync(recursive: true);

  // Copy App into framework directory.
  environment.buildDir
    .childDirectory('App.framework')
    .childFile('App')
    .copySync(outputDirectory.childFile('App').path);

  // Copy assets into asset directory.
  final Directory assetDirectory = outputDirectory
    .childDirectory('Resources')
    .childDirectory('flutter_assets');
  assetDirectory.createSync(recursive: true);

  // Copy Info.plist template.
  assetDirectory.parent.childFile('Info.plist')
    ..createSync()
    ..writeAsStringSync(r'''
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
<key>CFBundleDevelopmentRegion</key>
<string>en</string>
<key>CFBundleExecutable</key>
<string>App</string>
<key>CFBundleIdentifier</key>
<string>io.flutter.flutter.app</string>
<key>CFBundleInfoDictionaryVersion</key>
<string>6.0</string>
<key>CFBundleName</key>
<string>App</string>
<key>CFBundlePackageType</key>
<string>FMWK</string>
<key>CFBundleShortVersionString</key>
<string>1.0</string>
<key>CFBundleVersion</key>
<string>1.0</string>
</dict>
</plist>

''');
  if (buildMode == BuildMode.debug) {
    // Copy dill file.
    try {
      final File sourceFile = environment.buildDir.childFile('app.dill');
      sourceFile.copySync(assetDirectory.childFile('kernel_blob.bin').path);
    } catch (err) {
      throw Exception('Failed to copy app.dill: $err');
    }
    // Copy precompiled runtimes.
    try {
      final String vmSnapshotData = artifacts.getArtifactPath(Artifact.vmSnapshotData,
          platform: TargetPlatform.darwin_x64, mode: BuildMode.debug);
      final String isolateSnapshotData = artifacts.getArtifactPath(Artifact.isolateSnapshotData,
          platform: TargetPlatform.darwin_x64, mode: BuildMode.debug);
      fs.file(vmSnapshotData).copySync(
          assetDirectory.childFile('vm_snapshot_data').path);
      fs.file(isolateSnapshotData).copySync(
          assetDirectory.childFile('isolate_snapshot_data').path);
    } catch (err) {
      throw Exception('Failed to copy precompiled runtimes: $err');
    }
  }
  // Create symlink to current version. These must be relative, from the
  // framework root for Resources/App and from the versions root for
  // Current.
  try {
    final Link currentVersion = outputDirectory.parent
        .childLink('Current');
    if (!currentVersion.existsSync()) {
      final String linkPath = fs.path.relative(outputDirectory.path,
          from: outputDirectory.parent.path);
      currentVersion.createSync('$linkPath${fs.path.separator}');
    }
    // Create symlink to current resources.
    final Link currentResources = frameworkRootDirectory
        .childLink('Resources');
    if (!currentResources.existsSync()) {
      final String linkPath = fs.path.relative(fs.path.join(currentVersion.path, 'Resources'),
          from: frameworkRootDirectory.path);
      currentResources.createSync(linkPath);
    }
    // Create symlink to current binary.
    final Link currentFramework = frameworkRootDirectory
        .childLink('App');
    if (!currentFramework.existsSync()) {
      final String linkPath = fs.path.relative(fs.path.join(currentVersion.path, 'App'),
          from: frameworkRootDirectory.path);
      currentFramework.createSync(linkPath);
    }
  } on FileSystemException {
    throw Exception('Failed to create symlinks for framework. try removing '
      'the "${environment.outputDir.path}" directory and rerunning');
  }
}
