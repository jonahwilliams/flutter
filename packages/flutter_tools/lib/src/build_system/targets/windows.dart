// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import '../../artifacts.dart';
import '../../base/file_system.dart';
import '../../build_info.dart';
import '../build_system.dart';
import '../depfile.dart';

import 'common.dart';
import 'desktop.dart';

/// The only files/subdirectories we care about.
const List<String> _kWindowsArtifacts = <String>[
  'flutter_windows.dll',
  'flutter_windows.dll.exp',
  'flutter_windows.dll.lib',
  'flutter_windows.dll.pdb',
  'flutter_export.h',
  'flutter_messenger.h',
  'flutter_plugin_registrar.h',
  'flutter_windows.h',
  'icudtl.dat',
];

const String _kWindowsDepfile = 'windows_engine_sources.d';

/// Copies the Windows desktop embedding files to the copy directory.
class UnpackWindows extends Target {
  const UnpackWindows();

  @override
  String get name => 'unpack_windows';

  @override
  List<Source> get inputs => const <Source>[
    Source.pattern('{FLUTTER_ROOT}/packages/flutter_tools/lib/src/build_system/targets/windows.dart'),
  ];

  @override
  List<Source> get outputs => const <Source>[];

  @override
  List<String> get depfiles => const <String>[_kWindowsDepfile];

  @override
  List<Target> get dependencies => const <Target>[];

  @override
  Future<void> build(Environment environment) async {
    final BuildMode buildMode = getBuildModeForName(environment.defines[kBuildMode]);
    final String engineSourcePath = environment.artifacts
      .getArtifactPath(
        Artifact.windowsDesktopPath,
        platform: TargetPlatform.windows_x64,
        mode: buildMode,
      );
    final String clientSourcePath = environment.artifacts
      .getArtifactPath(
        Artifact.windowsCppClientWrapper,
        platform: TargetPlatform.windows_x64,
        mode: buildMode,
      );
    final Directory outputDirectory = environment.fileSystem.directory(
      environment.fileSystem.path.join(
        environment.projectDir.path,
        'windows',
        'flutter',
        'ephemeral',
      ),
    );
    final Depfile depfile = unpackDesktopArtifacts(
      fileSystem: environment.fileSystem,
      artifacts: _kWindowsArtifacts,
      engineSourcePath: engineSourcePath,
      outputDirectory: outputDirectory,
      clientSourcePath: clientSourcePath,
    );
    final DepfileService depfileService = DepfileService(
      fileSystem: environment.fileSystem,
      logger: environment.logger,
    );
    depfileService.writeToFile(
      depfile,
      environment.buildDir.childFile(_kWindowsDepfile),
    );
  }
}

/// Creates a debug bundle for the Windows desktop target.
class DebugBundleWindowsAssets extends ApplicationAssetBundle {
  const DebugBundleWindowsAssets();

  @override
  List<Source> get inputs => <Source>[
    ...super.inputs,
    const Source.artifact(Artifact.vmSnapshotData, mode: BuildMode.debug),
    const Source.artifact(Artifact.isolateSnapshotData, mode: BuildMode.debug),
  ];

  @override
  List<Source> get outputs => <Source>[
    ...super.outputs,
    const Source.pattern('{OUTPUT_DIR}/vm_snapshot_data'),
    const Source.pattern('{OUTPUT_DIR}/isolate_snapshot_data'),
    const Source.pattern('{OUTPUT_DIR}/kernel_blob.bin'),
  ];

  @override
  String get name => 'debug_bundle_windows_assets';

  @override
  List<Target> get dependencies => const <Target>[
    UnpackWindows(),
  ];
}

class ProfileBundleWindowsAssets extends ApplicationAssetBundle
  with ReleaseAssetBundle {

  const ProfileBundleWindowsAssets();

  @override
  String get name => 'profile_bundle_windows_assets';

  @override
  List<Source> get inputs => <Source>[
    ...super.inputs,
    const Source.pattern('{BUILD_DIR}/app.so'),
  ];

  @override
  List<Source> get outputs => <Source>[
    ...super.outputs,
    const Source.pattern('{OUTPUT_DIR}/app.so')
  ];

  @override
  List<Target> get dependencies => const <Target>[
    AotElfProfile(),
    UnpackWindows(),
  ];
}

class ReleaseBundleWindowsAssets extends ApplicationAssetBundle
  with ReleaseAssetBundle {

  const ReleaseBundleWindowsAssets();

  @override
  String get name => 'release_bundle_windows_assets';


  @override
  List<Source> get inputs => <Source>[
    ...super.inputs,
    const Source.pattern('{BUILD_DIR}/app.so'),
  ];

  @override
  List<Source> get outputs => <Source>[
    ...super.outputs,
    const Source.pattern('{OUTPUT_DIR}/app.so')
  ];

  @override
  List<Target> get dependencies => const <Target>[
    AotElfRelease(),
    UnpackWindows(),
  ];
}
