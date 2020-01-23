// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';

import 'package:flutter_tools/src/android/android_workflow.dart';
import 'package:flutter_tools/src/base/common.dart';
import 'package:flutter_tools/src/base/logger.dart';
import 'package:flutter_tools/src/base/utils.dart';
import 'package:flutter_tools/src/commands/daemon.dart';
import 'package:flutter_tools/src/fuchsia/fuchsia_workflow.dart';
import 'package:flutter_tools/src/globals.dart' as globals;
import 'package:flutter_tools/src/ios/ios_workflow.dart';
import 'package:flutter_tools/src/resident_runner.dart';

import '../../src/common.dart';
import '../../src/context.dart';
import '../../src/mocks.dart';

void main() {
  Daemon daemon;
  NotifyingLogger notifyingLogger;

  group('daemon', () {
    setUp(() {
      notifyingLogger = NotifyingLogger();
    });

    tearDown(() {
      if (daemon != null) {
        return daemon.shutdown();
      }
      notifyingLogger.dispose();
    });

    testUsingContext('daemon.version command should succeed', () async {
      final StreamController<Map<String, dynamic>> commands = StreamController<Map<String, dynamic>>();
      final StreamController<Map<String, dynamic>> responses = StreamController<Map<String, dynamic>>();
      daemon = Daemon(
        commands.stream,
        responses.add,
        notifyingLogger: notifyingLogger,
        dartDefines: const <String>[],
      );
      commands.add(<String, dynamic>{'id': 0, 'method': 'daemon.version'});
      final Map<String, dynamic> response = await responses.stream.firstWhere(_notEvent);
      expect(response['id'], 0);
      expect(response['result'], isNotEmpty);
      expect(response['result'] is String, true);
      await responses.close();
      await commands.close();
    });

    testUsingContext('printError should send daemon.logMessage event', () async {
      final StreamController<Map<String, dynamic>> commands = StreamController<Map<String, dynamic>>();
      final StreamController<Map<String, dynamic>> responses = StreamController<Map<String, dynamic>>();
      daemon = Daemon(
        commands.stream,
        responses.add,
        notifyingLogger: notifyingLogger,
        dartDefines: const <String>[],
      );
      globals.printError('daemon.logMessage test');
      final Map<String, dynamic> response = await responses.stream.firstWhere((Map<String, dynamic> map) {
        return map['event'] == 'daemon.logMessage' && map['params']['level'] == 'error';
      });
      expect(response['id'], isNull);
      expect(response['event'], 'daemon.logMessage');
      final Map<String, String> logMessage = castStringKeyedMap(response['params']).cast<String, String>();
      expect(logMessage['level'], 'error');
      expect(logMessage['message'], 'daemon.logMessage test');
      await responses.close();
      await commands.close();
    }, overrides: <Type, Generator>{
      Logger: () => notifyingLogger,
    });

    testUsingContext('printStatus should log to stdout when logToStdout is enabled', () async {
      final StringBuffer buffer = StringBuffer();

      await runZoned<Future<void>>(() async {
        final StreamController<Map<String, dynamic>> commands = StreamController<Map<String, dynamic>>();
        final StreamController<Map<String, dynamic>> responses = StreamController<Map<String, dynamic>>();
        daemon = Daemon(
          commands.stream,
          responses.add,
          notifyingLogger: notifyingLogger,
          logToStdout: true,
          dartDefines: const <String>[],
        );
        globals.printStatus('daemon.logMessage test');
        // Service the event loop.
        await Future<void>.value();
      }, zoneSpecification: ZoneSpecification(print: (Zone self, ZoneDelegate parent, Zone zone, String line) {
        buffer.writeln(line);
      }));

      expect(buffer.toString().trim(), 'daemon.logMessage test');
    }, overrides: <Type, Generator>{
      Logger: () => notifyingLogger,
    });

    testUsingContext('daemon.shutdown command should stop daemon', () async {
      final StreamController<Map<String, dynamic>> commands = StreamController<Map<String, dynamic>>();
      final StreamController<Map<String, dynamic>> responses = StreamController<Map<String, dynamic>>();
      daemon = Daemon(
        commands.stream,
        responses.add,
        notifyingLogger: notifyingLogger,
        dartDefines: const <String>[],
      );
      commands.add(<String, dynamic>{'id': 0, 'method': 'daemon.shutdown'});
      return daemon.onExit.then<void>((int code) async {
        await commands.close();
        expect(code, 0);
      });
    });

    testUsingContext('app.restart without an appId should report an error', () async {
      final StreamController<Map<String, dynamic>> commands = StreamController<Map<String, dynamic>>();
      final StreamController<Map<String, dynamic>> responses = StreamController<Map<String, dynamic>>();
      daemon = Daemon(
        commands.stream,
        responses.add,
        notifyingLogger: notifyingLogger,
        dartDefines: const <String>[],
      );

      commands.add(<String, dynamic>{'id': 0, 'method': 'app.restart'});
      final Map<String, dynamic> response = await responses.stream.firstWhere(_notEvent);
      expect(response['id'], 0);
      expect(response['error'], contains('appId is required'));
      await responses.close();
      await commands.close();
    });

    testUsingContext('ext.flutter.debugPaint via service extension without an appId should report an error', () async {
      final StreamController<Map<String, dynamic>> commands = StreamController<Map<String, dynamic>>();
      final StreamController<Map<String, dynamic>> responses = StreamController<Map<String, dynamic>>();
      daemon = Daemon(
        commands.stream,
        responses.add,
        notifyingLogger: notifyingLogger,
        dartDefines: const <String>[],
      );

      commands.add(<String, dynamic>{
        'id': 0,
        'method': 'app.callServiceExtension',
        'params': <String, String>{
          'methodName': 'ext.flutter.debugPaint',
        },
      });
      final Map<String, dynamic> response = await responses.stream.firstWhere(_notEvent);
      expect(response['id'], 0);
      expect(response['error'], contains('appId is required'));
      await responses.close();
      await commands.close();
    });

    testUsingContext('app.stop without appId should report an error', () async {
      final StreamController<Map<String, dynamic>> commands = StreamController<Map<String, dynamic>>();
      final StreamController<Map<String, dynamic>> responses = StreamController<Map<String, dynamic>>();
      daemon = Daemon(
        commands.stream,
        responses.add,
        notifyingLogger: notifyingLogger,
        dartDefines: const <String>[],
      );

      commands.add(<String, dynamic>{'id': 0, 'method': 'app.stop'});
      final Map<String, dynamic> response = await responses.stream.firstWhere(_notEvent);
      expect(response['id'], 0);
      expect(response['error'], contains('appId is required'));
      await responses.close();
      await commands.close();
    });

    testUsingContext('device.getDevices should respond with list', () async {
      final StreamController<Map<String, dynamic>> commands = StreamController<Map<String, dynamic>>();
      final StreamController<Map<String, dynamic>> responses = StreamController<Map<String, dynamic>>();
      daemon = Daemon(
        commands.stream,
        responses.add,
        notifyingLogger: notifyingLogger,
        dartDefines: const <String>[],
      );
      commands.add(<String, dynamic>{'id': 0, 'method': 'device.getDevices'});
      final Map<String, dynamic> response = await responses.stream.firstWhere(_notEvent);
      expect(response['id'], 0);
      expect(response['result'], isList);
      await responses.close();
      await commands.close();
    });

    testUsingContext('device.getDevices reports available devices', () async {
      final StreamController<Map<String, dynamic>> commands = StreamController<Map<String, dynamic>>();
      final StreamController<Map<String, dynamic>> responses = StreamController<Map<String, dynamic>>();
      daemon = Daemon(
        commands.stream,
        responses.add,
        notifyingLogger: notifyingLogger,
        dartDefines: const <String>[],
      );
      final MockPollingDeviceDiscovery discoverer = MockPollingDeviceDiscovery();
      daemon.deviceDomain.addDeviceDiscoverer(discoverer);
      discoverer.addDevice(MockAndroidDevice());
      commands.add(<String, dynamic>{'id': 0, 'method': 'device.getDevices'});
      final Map<String, dynamic> response = await responses.stream.firstWhere(_notEvent);
      expect(response['id'], 0);
      final dynamic result = response['result'];
      expect(result, isList);
      expect(result, isNotEmpty);
      await responses.close();
      await commands.close();
    });

    testUsingContext('should send device.added event when device is discovered', () async {
      final StreamController<Map<String, dynamic>> commands = StreamController<Map<String, dynamic>>();
      final StreamController<Map<String, dynamic>> responses = StreamController<Map<String, dynamic>>();
      daemon = Daemon(
        commands.stream,
        responses.add,
        notifyingLogger: notifyingLogger,
        dartDefines: const <String>[],
      );

      final MockPollingDeviceDiscovery discoverer = MockPollingDeviceDiscovery();
      daemon.deviceDomain.addDeviceDiscoverer(discoverer);
      discoverer.addDevice(MockAndroidDevice());

      return await responses.stream.skipWhile(_isConnectedEvent).first.then<void>((Map<String, dynamic> response) async {
        expect(response['event'], 'device.added');
        expect(response['params'], isMap);

        final Map<String, dynamic> params = castStringKeyedMap(response['params']);
        expect(params['platform'], isNotEmpty); // the mock device has a platform of 'android-arm'

        await responses.close();
        await commands.close();
      });
    }, overrides: <Type, Generator>{
      AndroidWorkflow: () => MockAndroidWorkflow(),
      IOSWorkflow: () => MockIOSWorkflow(),
      FuchsiaWorkflow: () => MockFuchsiaWorkflow(),
    });

    testUsingContext('emulator.launch without an emulatorId should report an error', () async {
      final StreamController<Map<String, dynamic>> commands = StreamController<Map<String, dynamic>>();
      final StreamController<Map<String, dynamic>> responses = StreamController<Map<String, dynamic>>();
      daemon = Daemon(
        commands.stream,
        responses.add,
        notifyingLogger: notifyingLogger,
        dartDefines: const <String>[],
      );

      commands.add(<String, dynamic>{'id': 0, 'method': 'emulator.launch'});
      final Map<String, dynamic> response = await responses.stream.firstWhere(_notEvent);
      expect(response['id'], 0);
      expect(response['error'], contains('emulatorId is required'));
      await responses.close();
      await commands.close();
    });

    testUsingContext('emulator.getEmulators should respond with list', () async {
      final StreamController<Map<String, dynamic>> commands = StreamController<Map<String, dynamic>>();
      final StreamController<Map<String, dynamic>> responses = StreamController<Map<String, dynamic>>();
      daemon = Daemon(
        commands.stream,
        responses.add,
        notifyingLogger: notifyingLogger,
        dartDefines: const <String>[],
      );
      commands.add(<String, dynamic>{'id': 0, 'method': 'emulator.getEmulators'});
      final Map<String, dynamic> response = await responses.stream.firstWhere(_notEvent);
      expect(response['id'], 0);
      expect(response['result'], isList);
      await responses.close();
      await commands.close();
    });

    testUsingContext('daemon can send exposeUrl requests to the client', () async {
      const String originalUrl = 'http://localhost:1234/';
      const String mappedUrl = 'https://publichost:4321/';
      final StreamController<Map<String, dynamic>> input = StreamController<Map<String, dynamic>>();
      final StreamController<Map<String, dynamic>> output = StreamController<Map<String, dynamic>>();

      daemon = Daemon(
        input.stream,
        output.add,
        notifyingLogger: notifyingLogger,
        dartDefines: const <String>[],
      );

      // Respond to any requests from the daemon to expose a URL.
      unawaited(output.stream
        .firstWhere((Map<String, dynamic> request) => request['method'] == 'app.exposeUrl')
        .then((Map<String, dynamic> request) {
          expect(request['params']['url'], equals(originalUrl));
          input.add(<String, dynamic>{'id': request['id'], 'result': <String, dynamic>{'url': mappedUrl}});
        })
      );

      final String exposedUrl = await daemon.daemonDomain.exposeUrl(originalUrl);
      expect(exposedUrl, equals(mappedUrl));

      await output.close();
      await input.close();
    });
  });

  group('daemon serialization', () {
    test('OperationResult', () {
      expect(
        jsonEncodeObject(OperationResult.ok),
        '{"code":0,"message":""}',
      );
      expect(
        jsonEncodeObject(OperationResult(1, 'foo')),
        '{"code":1,"message":"foo"}',
      );
    });
  });
}

bool _notEvent(Map<String, dynamic> map) => map['event'] == null;

bool _isConnectedEvent(Map<String, dynamic> map) => map['event'] == 'daemon.connected';

class MockFuchsiaWorkflow extends FuchsiaWorkflow {
  MockFuchsiaWorkflow({ this.canListDevices = true });

  @override
  final bool canListDevices;
}

class MockAndroidWorkflow extends AndroidWorkflow {
  MockAndroidWorkflow({ this.canListDevices = true });

  @override
  final bool canListDevices;
}

class MockIOSWorkflow extends IOSWorkflow {
  MockIOSWorkflow({ this.canListDevices = true });

  @override
  final bool canListDevices;
}
