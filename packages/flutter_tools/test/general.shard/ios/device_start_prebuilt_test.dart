// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';

import 'package:file/memory.dart';
import 'package:flutter_tools/src/application_package.dart';
import 'package:flutter_tools/src/base/file_system.dart';
import 'package:flutter_tools/src/base/logger.dart';
import 'package:flutter_tools/src/build_info.dart';
import 'package:flutter_tools/src/device.dart';
import 'package:flutter_tools/src/ios/devices.dart';
import 'package:flutter_tools/src/ios/ios_deploy.dart';
import 'package:flutter_tools/src/mdns_discovery.dart';
import 'package:flutter_tools/src/reporting/reporting.dart';
import 'package:meta/meta.dart';
import 'package:mockito/mockito.dart';
import 'package:platform/platform.dart';
import 'package:flutter_tools/src/globals.dart' as globals;

import '../../src/common.dart';
import '../../src/context.dart';
import '../../src/mocks.dart';

final IOSApp iosApp = PrebuiltIOSApp(projectBundleId: 'app', bundleName: 'Runner');

void main() {
  testUsingContext('disposing device disposes the portForwarder and logReader', () async {
    final IOSDevice device = setUpIOSDevice(sdkVersion: '13.0.1');
    final DevicePortForwarder devicePortForwarder = MockDevicePortForwarder();
    final DeviceLogReader deviceLogReader = MockDeviceLogReader();

    device.portForwarder = devicePortForwarder;
    device.setLogReader(iosApp, deviceLogReader);
    await device.dispose();

    verify(deviceLogReader.dispose()).called(1);
    verify(devicePortForwarder.dispose()).called(1);
  });

  // Still uses context for analytics and Mdns.
  testUsingContext('IOSDevice.startApp succeeds in debug mode via mDNS discovery', () async {
    final FakeProcessManager processManager = FakeProcessManager.list(<FakeCommand>[

    ]);
    final IOSDevice device = setUpIOSDevice(sdkVersion: '13.3', processManager: processManager);
    device.portForwarder = const NoOpDevicePortForwarder();
    device.setLogReader(iosApp, FakeDeviceLogReader());
    final Uri uri = Uri(
      scheme: 'http',
      host: '127.0.0.1',
      port: 1234,
      path: 'observatory',
    );
    when(MDnsObservatoryDiscovery.instance.getObservatoryUri(any, any, usesIpv6: anyNamed('usesIpv6')))
      .thenAnswer((Invocation invocation) async => uri);

    final LaunchResult launchResult = await device.startApp(iosApp,
      prebuiltApplication: true,
      debuggingOptions: DebuggingOptions.enabled(BuildInfo.debug),
      platformArgs: <String, dynamic>{},
    );

    verify(globals.flutterUsage.sendEvent('ios-handshake', 'mdns-success')).called(1);
    expect(launchResult.started, true);
    expect(launchResult.hasObservatory, true);
    expect(await device.stopApp(iosApp), false);
  }, overrides: <Type, Generator>{
    MDnsObservatoryDiscovery: () => MockMDnsObservatoryDiscovery(),
    Usage: () => MockUsage(),
  });

  // Still uses context for analytics and Mdns.
  testUsingContext('IOSDevice.startAppsucceeds in debug mode when mDNS fails by falling back to manual protocol discovery', () async {
    final FakeProcessManager processManager = FakeProcessManager.list(<FakeCommand>[]);
    final IOSDevice device = setUpIOSDevice(
      sdkVersion: '13.3',
      processManager: processManager,
    );
    final FakeDeviceLogReader deviceLogReader = FakeDeviceLogReader();
    device.portForwarder = const NoOpDevicePortForwarder();
    device.setLogReader(iosApp, deviceLogReader);

    // Now that the reader is used, start writing messages to it.
    Timer.run(() {
      deviceLogReader.addLine('Foo');
      deviceLogReader.addLine('Observatory listening on http://127.0.0.1:456');
    });
    when(MDnsObservatoryDiscovery.instance.getObservatoryUri(any, any, usesIpv6: anyNamed('usesIpv6')))
      .thenAnswer((Invocation invocation) async => null);

    final LaunchResult launchResult = await device.startApp(iosApp,
      prebuiltApplication: true,
      debuggingOptions: DebuggingOptions.enabled(BuildInfo.debug),
      platformArgs: <String, dynamic>{},
    );

    expect(launchResult.started, true);
    expect(launchResult.hasObservatory, true);
    verify(globals.flutterUsage.sendEvent('ios-handshake', 'mdns-failure')).called(1);
    verify(globals.flutterUsage.sendEvent('ios-handshake', 'fallback-success')).called(1);
    expect(await device.stopApp(iosApp), false);
  }, overrides: <Type, Generator>{
    Usage: () => MockUsage(),
    MDnsObservatoryDiscovery: () => MockMDnsObservatoryDiscovery(),
  });

  // Still uses context for analytics and Mdns.
  testUsingContext('IOSDevice.startApp fails in debug mode when mDNS fails and when Observatory URI is malformed', () async {
    final FakeProcessManager processManager = FakeProcessManager.list(<FakeCommand>[]);
    final IOSDevice device = setUpIOSDevice(
      sdkVersion: '13.3',
      processManager: processManager,
    );
    final FakeDeviceLogReader deviceLogReader = FakeDeviceLogReader();
    device.portForwarder = const NoOpDevicePortForwarder();
    device.setLogReader(iosApp, deviceLogReader);

    // Now that the reader is used, start writing messages to it.
    Timer.run(() {
      deviceLogReader.addLine('Foo');
      deviceLogReader.addLine('Observatory listening on http:/:/127.0.0.1:456');
    });
    when(MDnsObservatoryDiscovery.instance.getObservatoryUri(any, any, usesIpv6: anyNamed('usesIpv6')))
      .thenAnswer((Invocation invocation) async => null);

    final LaunchResult launchResult = await device.startApp(iosApp,
      prebuiltApplication: true,
      debuggingOptions: DebuggingOptions.enabled(BuildInfo.debug),
      platformArgs: <String, dynamic>{},
    );

    expect(launchResult.started, false);
    expect(launchResult.hasObservatory, false);
    verify(globals.flutterUsage.sendEvent(
      'ios-handshake',
      'failure-other',
      label: anyNamed('label'),
      value: anyNamed('value'),
    )).called(1);
    verify(globals.flutterUsage.sendEvent('ios-handshake', 'mdns-failure')).called(1);
    verify(globals.flutterUsage.sendEvent('ios-handshake', 'fallback-failure')).called(1);
    }, overrides: <Type, Generator>{
      MDnsObservatoryDiscovery: () => MockMDnsObservatoryDiscovery(),
      Usage: () => MockUsage(),
    });

  testUsingContext('IOSDevice.startApp succeeds in release mode', () async {
    final FakeProcessManager processManager = FakeProcessManager.list(<FakeCommand>[]);
    final IOSDevice device = setUpIOSDevice(
      sdkVersion: '13.3',
      processManager: processManager,
    );

    final LaunchResult launchResult = await device.startApp(iosApp,
      prebuiltApplication: true,
      debuggingOptions: DebuggingOptions.disabled(BuildInfo.release),
      platformArgs: <String, dynamic>{},
    );

    expect(launchResult.started, false);
    expect(launchResult.hasObservatory, false);
    expect(await device.stopApp(iosApp), false);
    expect(processManager.hasRemainingExpectations, false);
  });

  testUsingContext('IOSDevice.startApp forwards all supported debugging options', () async {
    final FakeProcessManager processManager = FakeProcessManager.list(<FakeCommand>[

    ]);
    final IOSDevice device = setUpIOSDevice(
      sdkVersion: '13.3',
      processManager: processManager,
    );
    device.setLogReader(iosApp, FakeDeviceLogReader());
    final Uri uri = Uri(
      scheme: 'http',
      host: '127.0.0.1',
      port: 1234,
      path: 'observatory',
    );
    when(MDnsObservatoryDiscovery.instance.getObservatoryUri(any, any, usesIpv6: anyNamed('usesIpv6')))
      .thenAnswer((Invocation invocation) async => uri);

    final LaunchResult launchResult = await device.startApp(iosApp,
      prebuiltApplication: true,
      debuggingOptions: DebuggingOptions.enabled(
        BuildInfo.debug,
        startPaused: true,
        disableServiceAuthCodes: true,
        dartFlags: '--foo',
        enableSoftwareRendering: true,
        skiaDeterministicRendering: true,
        traceSkia: true,
        traceSystrace: true,
        endlessTraceBuffer: true,
        dumpSkpOnShaderCompilation: true,
        cacheSkSL: true,
        verboseSystemLogs: true,
        deviceVmServicePort: 123456
      ),
      platformArgs: <String, dynamic>{},
    );

    expect(launchResult.started, true);
    expect(await device.stopApp(iosApp), true);
    expect(processManager.hasRemainingExpectations, false);
  }, overrides: <Type, Generator>{
    MDnsObservatoryDiscovery: () => MockMDnsObservatoryDiscovery(),
    Usage: () => MockUsage(),
  });
}

IOSDevice setUpIOSDevice({
  @required String sdkVersion,
  FileSystem fileSystem,
  Logger logger,
  ProcessManager processManager,
}) {
  final FakePlatform macPlatform = FakePlatform(
    operatingSystem: 'macos'
  );
  return IOSDevice('123',
    name: 'iPhone 1',
    sdkVersion: sdkVersion,
    iproxyPath: 'iproxy',
    fileSystem: fileSystem ?? MemoryFileSystem.test(),
    platform: macPlatform,
    iosDeploy: IOSDeploy.test(
      logger: logger ?? BufferLogger.test(),
      platform: macPlatform,
      processManager: processManager ?? FakeProcessManager.any(),
    ),
    cpuArchitecture: DarwinArch.arm64,
  );
}

class MockDevicePortForwarder extends Mock implements DevicePortForwarder {}
class MockDeviceLogReader extends Mock implements DeviceLogReader  {}
class MockUsage extends Mock implements Usage {}
class MockMDnsObservatoryDiscovery extends Mock implements MDnsObservatoryDiscovery {}