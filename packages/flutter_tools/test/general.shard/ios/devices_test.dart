// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:file/memory.dart';
import 'package:mockito/mockito.dart';
import 'package:platform/platform.dart';

import 'package:flutter_tools/src/application_package.dart';
import 'package:flutter_tools/src/base/logger.dart';
import 'package:flutter_tools/src/build_info.dart';
import 'package:flutter_tools/src/device.dart';
import 'package:flutter_tools/src/ios/devices.dart';
import 'package:flutter_tools/src/ios/ios_deploy.dart';

import '../../src/common.dart';
import '../../src/context.dart';

void main() {
  testWithoutContext('IOSDevice can be created only on macOS', () {
    expect(setUpIOSDevice(), returnsNormally);
    expect(() => setUpIOSDevice(platform: FakePlatform(operatingSystem: 'linux')),
      throwsAssertionError);
    expect(() => setUpIOSDevice(platform: FakePlatform(operatingSystem: 'windows')),
      throwsAssertionError);
  });

  testWithoutContext('IOSDevice parses major version', () {
    expect(setUpIOSDevice(sdkVersion: '1.0.0').majorSdkVersion, 1);
    expect(setUpIOSDevice(sdkVersion: '13.1.0').majorSdkVersion, 13);
    expect(setUpIOSDevice(sdkVersion: '10.0.0').majorSdkVersion, 10);
    expect(setUpIOSDevice(sdkVersion: '0').majorSdkVersion, 0);
    expect(setUpIOSDevice(sdkVersion: 'bogus').majorSdkVersion, 0);
  });

  testWithoutContext('IOSDevice.dispose kills all log readers & port forwarders', () async {
    final IOSDevice iosDevice = setUpIOSDevice();
    final IOSApp iosApp = PrebuiltIOSApp(projectBundleId: 'app');
    final MockDevicePortForwarder portForwarderA = MockDevicePortForwarder();
    final MockDeviceLogReader logReaderA = MockDeviceLogReader();
    final MockDeviceLogReader logReaderB = MockDeviceLogReader();

    iosDevice.setLogReader(iosApp, logReaderA);
    iosDevice.setLogReader(iosApp, logReaderB);
    iosDevice.portForwarder = portForwarderA;

    await iosDevice.dispose();

    verify(logReaderA.dispose()).called(1);
    verify(logReaderB.dispose()).called(1);
    verify(portForwarderA.dispose()).called(1);
  });
}

IOSDevice setUpIOSDevice({
  Platform platform,
  String sdkVersion = '13.0.0',
}) {
  final FakePlatform macPlatform = FakePlatform(
   operatingSystem: 'macos',
   environment: <String, String>{},
 );
  return IOSDevice(
    'device-123',
    logger: BufferLogger.test(),
    fileSystem: MemoryFileSystem.test(),
    platform: platform ?? macPlatform,
    iosDeploy: IOSDeploy.test(
      logger: BufferLogger.test(),
      platform: platform ?? macPlatform,
      processManager: FakeProcessManager.any(),
    ),
    name: 'iPhone 1',
    cpuArchitecture: DarwinArch.arm64,
    sdkVersion: sdkVersion,
    iproxyPath: 'iproxy',
  );
}

class MockDevicePortForwarder extends Mock implements DevicePortForwarder {}
class MockDeviceLogReader extends Mock implements DeviceLogReader {}
