// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:file/memory.dart';
import 'package:flutter_tools/src/application_package.dart';
import 'package:flutter_tools/src/base/file_system.dart';
import 'package:flutter_tools/src/base/logger.dart';
import 'package:flutter_tools/src/build_info.dart';
import 'package:flutter_tools/src/device.dart';
import 'package:flutter_tools/src/ios/devices.dart';
import 'package:flutter_tools/src/ios/ios_deploy.dart';
import 'package:flutter_tools/src/project.dart';
import 'package:meta/meta.dart';
import 'package:platform/platform.dart';

import '../../src/common.dart';
import '../../src/context.dart';

void main() {
  testUsingContext('non-prebuilt succeeds in debug mode flaky: false', () async {
    final FileSystem fileSystem = MemoryFileSystem.test();
    fileSystem.directory('ios').createSync();
    final IOSApp app = await AbsoluteBuildableIOSApp.fromProject(
      FlutterProject.fromDirectory(fileSystem.currentDirectory).ios);
    final FakeProcessManager processManager = FakeProcessManager.list(<FakeCommand>[

    ]);
    final IOSDevice device = setUpIOSDevice(
      processManager: processManager,
      fileSystem: fileSystem,
    );
    final LaunchResult launchResult = await device.startApp(
      app,
      prebuiltApplication: false,
      debuggingOptions: DebuggingOptions.disabled(BuildInfo.debug),
      platformArgs: <String, dynamic>{},
    );

    expect(launchResult.started, true);
    expect(launchResult.hasObservatory, false);
  });

  testUsingContext('non-prebuilt succeeds in debug mode flaky: true', () async {
    final FileSystem fileSystem = MemoryFileSystem.test();
    fileSystem.directory('ios').createSync();
    final IOSApp app = await AbsoluteBuildableIOSApp.fromProject(
      FlutterProject.fromDirectory(fileSystem.currentDirectory).ios);
    final FakeProcessManager processManager = FakeProcessManager.list(<FakeCommand>[

    ]);
    final IOSDevice device = setUpIOSDevice(
      processManager: processManager,
      fileSystem: fileSystem,
    );
    final LaunchResult launchResult = await device.startApp(
      app,
      prebuiltApplication: false,
      debuggingOptions: DebuggingOptions.disabled(BuildInfo.debug),
      platformArgs: <String, dynamic>{},
    );

    expect(launchResult.started, true);
    expect(launchResult.hasObservatory, false);
  });

  testUsingContext('non-prebuilt succeeds in debug mode with concurrent build failiure', () async {
    final FileSystem fileSystem = MemoryFileSystem.test();
    fileSystem.directory('ios').createSync();
    final IOSApp app = await AbsoluteBuildableIOSApp.fromProject(
      FlutterProject.fromDirectory(fileSystem.currentDirectory).ios);
    final FakeProcessManager processManager = FakeProcessManager.list(<FakeCommand>[

    ]);
    final IOSDevice device = setUpIOSDevice(
      processManager: processManager,
      fileSystem: fileSystem,
    );
    final LaunchResult launchResult = await device.startApp(
      app,
      prebuiltApplication: false,
      debuggingOptions: DebuggingOptions.disabled(BuildInfo.debug),
      platformArgs: <String, dynamic>{},
    );

    expect(launchResult.started, true);
    expect(launchResult.hasObservatory, false);
  });
}

class AbsoluteBuildableIOSApp extends BuildableIOSApp {
  AbsoluteBuildableIOSApp(IosProject project, String projectBundleId) :
    super(project, projectBundleId);

  static Future<AbsoluteBuildableIOSApp> fromProject(IosProject project) async {
    final String projectBundleId = await project.productBundleIdentifier;
    return AbsoluteBuildableIOSApp(project, projectBundleId);
  }

  @override
  String get deviceBundlePath {
    final FileSystem fileSystem = project.parent.directory.fileSystem;
    return fileSystem.path.join(project.parent.directory.path, 'build', 'ios', 'iphoneos', name);
  }
}

//  void testNonPrebuilt(
//   String name, {
//   @required bool showBuildSettingsFlakes,
//   void Function() additionalSetup,
//   void Function() additionalExpectations,
// }) {
//   testUsingContext('non-prebuilt succeeds in debug mode $name', () async {
//     final Directory targetBuildDir =
//         projectDir.childDirectory('build/ios/iphoneos/Debug-arm64');

//     // The -showBuildSettings calls have a timeout and so go through
//     // globals.processManager.start().
//     mockProcessManager.processFactory = flakyProcessFactory(
//       flakes: showBuildSettingsFlakes ? 1 : 0,
//       delay: const Duration(seconds: 62),
//       filter: (List<String> args) => args.contains('-showBuildSettings'),
//       stdout:
//           () => Stream<String>
//             .fromIterable(
//                 <String>['TARGET_BUILD_DIR = ${targetBuildDir.path}\n'])
//             .transform(utf8.encoder),
//     );

//     // Make all other subcommands succeed.
//     when(mockProcessManager.run(
//         any,
//         workingDirectory: anyNamed('workingDirectory'),
//         environment: anyNamed('environment'),
//     )).thenAnswer((Invocation inv) {
//       return Future<ProcessResult>.value(ProcessResult(0, 0, '', ''));
//     });

//     when(mockProcessManager.run(
//       argThat(contains('find-identity')),
//       environment: anyNamed('environment'),
//       workingDirectory: anyNamed('workingDirectory'),
//     )).thenAnswer((_) => Future<ProcessResult>.value(ProcessResult(
//           1, // pid
//           0, // exitCode
//           '''
// 1) 86f7e437faa5a7fce15d1ddcb9eaeaea377667b8 "iPhone Developer: Profile 1 (1111AAAA11)"
// 2) da4b9237bacccdf19c0760cab7aec4a8359010b0 "iPhone Developer: Profile 2 (2222BBBB22)"
// 3) 5bf1fd927dfb8679496a2e6cf00cbe50c1c87145 "iPhone Developer: Profile 3 (3333CCCC33)"
//   3 valid identities found''',
//           '',
//     )));

//     // Deploy works.
//     when(mockIosDeploy.runApp(
//       deviceId: anyNamed('deviceId'),
//       bundlePath: anyNamed('bundlePath'),
//       launchArguments: anyNamed('launchArguments'),
//     )).thenAnswer((_) => Future<int>.value(0));

//     // Create a dummy project to avoid mocking out the whole directory
//     // structure expected by device.startApp().
//     Cache.flutterRoot = '../..';
//     final CreateCommand command = CreateCommand();
//     final CommandRunner<void> runner = createTestCommandRunner(command);
//     await runner.run(<String>[
//       'create',
//       '--no-pub',
//       projectDir.path,
//     ]);

//     if (additionalSetup != null) {
//       additionalSetup();
//     }

//     final IOSApp app = await AbsoluteBuildableIOSApp.fromProject(
//       FlutterProject.fromDirectory(projectDir).ios);
//     final IOSDevice device = IOSDevice(
//       '123',
//       name: 'iPhone 1',
//       sdkVersion: '13.3',
//       iproxyPath: 'iproxy',
//       fileSystem: globals.fs,
//       platform: macPlatform,
//       iosDeploy: mockIosDeploy,
//       cpuArchitecture: DarwinArch.arm64,
//     );

//     // Pre-create the expected build products.
//     targetBuildDir.createSync(recursive: true);
//     projectDir.childDirectory('build/ios/iphoneos/Runner.app').createSync(recursive: true);

//     final Completer<LaunchResult> completer = Completer<LaunchResult>();
//     FakeAsync().run((FakeAsync time) {
//       device.startApp(
//         app,
//         prebuiltApplication: false,
//         debuggingOptions: DebuggingOptions.disabled(const BuildInfo(BuildMode.debug, null, treeShakeIcons: false)),
//         platformArgs: <String, dynamic>{},
//       ).then((LaunchResult result) {
//         completer.complete(result);
//       });
//       time.flushMicrotasks();
//       time.elapse(const Duration(seconds: 65));
//     });
//     final LaunchResult launchResult = await completer.future;
//     expect(launchResult.started, isTrue);
//     expect(launchResult.hasObservatory, isFalse);
//     expect(await device.stopApp(mockApp), isFalse);

//     if (additionalExpectations != null) {
//       additionalExpectations();
//     }
//   }, overrides: <Type, Generator>{
//     DoctorValidatorsProvider: () => FakeIosDoctorProvider(),
//     IMobileDevice: () => mockIMobileDevice,
//     Platform: () => macPlatform,
//     ProcessManager: () => mockProcessManager,
//   });
// }

// testNonPrebuilt('flaky: false', showBuildSettingsFlakes: false);
// testNonPrebuilt('flaky: true', showBuildSettingsFlakes: true);
// testNonPrebuilt('with concurrent build failiure',
//   showBuildSettingsFlakes: false,
//   additionalSetup: () {
//     int callCount = 0;
//     when(mockProcessManager.run(
//       argThat(allOf(
//         contains('xcodebuild'),
//         contains('-configuration'),
//         contains('Debug'),
//       )),
//       workingDirectory: anyNamed('workingDirectory'),
//       environment: anyNamed('environment'),
//     )).thenAnswer((Invocation inv) {
//       // Succeed after 2 calls.
//       if (++callCount > 2) {
//         return Future<ProcessResult>.value(ProcessResult(0, 0, '', ''));
//       }
//       // Otherwise fail with the Xcode concurrent error.
//       return Future<ProcessResult>.value(ProcessResult(
//         0,
//         1,
//         '''
//           "/Developer/Xcode/DerivedData/foo/XCBuildData/build.db":
//           database is locked
//           Possibly there are two concurrent builds running in the same filesystem location.
//           ''',
//         '',
//       ));
//     });
//   },
//   additionalExpectations: () {
//     expect(testLogger.statusText, contains('will retry in 2 seconds'));
//     expect(testLogger.statusText, contains('will retry in 4 seconds'));
//     expect(testLogger.statusText, contains('Xcode build done.'));
//   },
// ));

IOSDevice setUpIOSDevice({
  @required ProcessManager processManager,
  @required FileSystem fileSystem,
}) {
  final FakePlatform macPlatform = FakePlatform(
   operatingSystem: 'macos',
   environment: <String, String>{},
 );
  return IOSDevice(
    'device-123',
    fileSystem: fileSystem,
    platform: macPlatform,
    iosDeploy: IOSDeploy.test(
      logger: BufferLogger.test(),
      platform: macPlatform,
      processManager: processManager,
    ),
    name: 'iPhone 1',
    cpuArchitecture: DarwinArch.arm64,
    sdkVersion: '13.0.0',
    iproxyPath: 'iproxy',
  );
}