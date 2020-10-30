// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';

import 'package:file/file.dart';
import 'package:file/memory.dart';
import 'package:flutter_tools/src/base/io.dart';
import 'package:flutter_tools/src/base/logger.dart';
import 'package:flutter_tools/src/base/os.dart';
import 'package:flutter_tools/src/base/platform.dart';
import 'package:flutter_tools/src/doctor.dart';
import 'package:flutter_tools/src/project.dart';
import 'package:flutter_tools/src/reporting/reporting.dart';
import 'package:mockito/mockito.dart';

import '../src/common.dart';
import '../src/context.dart';

void main() {
  BufferLogger logger;
  FileSystem fs;
  MockUsage mockUsage;
  Platform platform;
  OperatingSystemUtils operatingSystemUtils;

  setUp(() async {
    logger = BufferLogger.test();
    fs = MemoryFileSystem.test();

    mockUsage = MockUsage();
    when(mockUsage.clientId).thenReturn('00000000-0000-4000-0000-000000000000');

    platform = FakePlatform(environment: <String, String>{}, operatingSystem: 'linux');
    operatingSystemUtils = OperatingSystemUtils(
      fileSystem: fs,
      logger: logger,
      platform: platform,
      processManager: FakeProcessManager.any(),
    );
  });

  // Future<void> verifyCrashReportSent(FakeHttpClient fakeHttpClient, {
  //   int crashes = 1,
  // }) async {
  //   // Verify that we sent the crash report.
  //   expect(fakeHttpClient.lastUrl, Uri(
  //     scheme: 'https',
  //     host: 'clients2.google.com',
  //     port: 443,
  //     path: '/cr/report',
  //     queryParameters: <String, String>{
  //       'product': 'Flutter_Tools',
  //       'version': 'test-version',
  //     },
  //   ));
  //
  //   expect(crashInfo.fields['uuid'], '00000000-0000-4000-0000-000000000000');
  //   expect(crashInfo.fields['product'], 'Flutter_Tools');
  //   expect(crashInfo.fields['version'], 'test-version');
  //   expect(crashInfo.fields['osName'], 'linux');
  //   expect(crashInfo.fields['osVersion'], 'Linux');
  //   expect(crashInfo.fields['type'], 'DartError');
  //   expect(crashInfo.fields['error_runtime_type'], 'StateError');
  //   expect(crashInfo.fields['error_message'], 'Bad state: Test bad state error');
  //   expect(crashInfo.fields['comments'], 'crash');
  //
  //   expect(logger.traceText, contains('Sending crash report to Google.'));
  //   expect(logger.traceText, contains('Crash report sent (report ID: test-report-id)'));
  // }

  testWithoutContext('CrashReporter.informUser provides basic instructions', () async {
    final CrashReporter crashReporter = CrashReporter(
      fileSystem: fs,
      logger: logger,
      flutterProjectFactory: FlutterProjectFactory(fileSystem: fs, logger: logger),
      client: FakeHttpClient(),
    );

    final File file = fs.file('flutter_00.log');

    await crashReporter.informUser(
      CrashDetails(
        command: 'arg1 arg2 arg3',
        error: Exception('Dummy exception'),
        stackTrace: StackTrace.current,
        doctorText: 'Fake doctor text'),
      file,
    );

    expect(logger.errorText, contains('A crash report has been written to ${file.path}.'));
    expect(logger.statusText, contains('https://github.com/flutter/flutter/issues/new'));
  });

  testWithoutContext('suppress analytics', () async {
    when(mockUsage.suppressAnalytics).thenReturn(true);
    final FakeHttpClient fakeHttpClient = FakeHttpClient();

    final CrashReportSender crashReportSender = CrashReportSender(
      client: fakeHttpClient, //CrashingCrashReportSender(const SocketException('no internets')),
      usage: mockUsage,
      platform: platform,
      logger: logger,
      operatingSystemUtils: operatingSystemUtils,
    );

    await crashReportSender.sendReport(
      error: StateError('Test bad state error'),
      stackTrace: null,
      getFlutterVersion: () => 'test-version',
      command: 'crash',
    );

    expect(logger.traceText, isEmpty);
  });

  group('allow analytics', () {
    setUp(() async {
      when(mockUsage.suppressAnalytics).thenReturn(false);
    });

    testWithoutContext('should send crash reports', () async {
      final FakeHttpClient fakeHttpClient = FakeHttpClient();

      final CrashReportSender crashReportSender = CrashReportSender(
        client: fakeHttpClient,
        usage: mockUsage,
        platform: platform,
        logger: logger,
        operatingSystemUtils: operatingSystemUtils,
      );

      await crashReportSender.sendReport(
        error: StateError('Test bad state error'),
        stackTrace: null,
        getFlutterVersion: () => 'test-version',
        command: 'crash',
      );

      expect(fakeHttpClient.lastUrl, Uri(
        scheme: 'https',
        host: 'clients2.google.com',
        port: 443,
        path: '/cr/report',
        queryParameters: <String, String>{
          'product': 'Flutter_Tools',
          'version': 'test-version',
        },
      ));
    });

    testWithoutContext('should print an explanatory message when there is a SocketException', () async {
      final FakeHttpClient fakeHttpClient = FakeHttpClient(const SocketException('no internets'));
      final CrashReportSender crashReportSender = CrashReportSender(
        client: fakeHttpClient,
        usage: mockUsage,
        platform: platform,
        logger: logger,
        operatingSystemUtils: operatingSystemUtils,
      );

      await crashReportSender.sendReport(
        error: StateError('Test bad state error'),
        stackTrace: null,
        getFlutterVersion: () => 'test-version',
        command: 'crash',
      );

      expect(logger.errorText, contains('Failed to send crash report due to a network error'));
    });

    testWithoutContext('should print an explanatory message when there is an HttpException', () async {
      final FakeHttpClient fakeHttpClient = FakeHttpClient(const HttpException('no internets'));
      final CrashReportSender crashReportSender = CrashReportSender(
        client: fakeHttpClient,
        usage: mockUsage,
        platform: platform,
        logger: logger,
        operatingSystemUtils: operatingSystemUtils,
      );

      await crashReportSender.sendReport(
        error: StateError('Test bad state error'),
        stackTrace: null,
        getFlutterVersion: () => 'test-version',
        command: 'crash',
      );

      expect(logger.errorText, contains('Failed to send crash report due to a network error'));
    });

    testWithoutContext('should send only one crash report when sent many times', () async {
      final FakeHttpClient fakeHttpClient = FakeHttpClient();

      final CrashReportSender crashReportSender = CrashReportSender(
        client: fakeHttpClient,
        usage: mockUsage,
        platform: platform,
        logger: logger,
        operatingSystemUtils: operatingSystemUtils,
      );

      await crashReportSender.sendReport(
        error: StateError('Test bad state error'),
        stackTrace: null,
        getFlutterVersion: () => 'test-version',
        command: 'crash',
      );

      await crashReportSender.sendReport(
        error: StateError('Test bad state error'),
        stackTrace: null,
        getFlutterVersion: () => 'test-version',
        command: 'crash',
      );

      await crashReportSender.sendReport(
        error: StateError('Test bad state error'),
        stackTrace: null,
        getFlutterVersion: () => 'test-version',
        command: 'crash',
      );

      await crashReportSender.sendReport(
        error: StateError('Test bad state error'),
        stackTrace: null,
        getFlutterVersion: () => 'test-version',
        command: 'crash',
      );

      expect(fakeHttpClient.calls, 1);
      expect(fakeHttpClient.lastUrl, Uri(
        scheme: 'https',
        host: 'clients2.google.com',
        port: 443,
        path: '/cr/report',
        queryParameters: <String, String>{
          'product': 'Flutter_Tools',
          'version': 'test-version',
        },
      ));
    });

    testWithoutContext('should not send a crash report if on a user-branch', () async {
      final FakeHttpClient fakeHttpClient = FakeHttpClient();

      final CrashReportSender crashReportSender = CrashReportSender(
        client: fakeHttpClient,
        usage: mockUsage,
        platform: platform,
        logger: logger,
        operatingSystemUtils: operatingSystemUtils,
      );

      await crashReportSender.sendReport(
        error: StateError('Test bad state error'),
        stackTrace: null,
        getFlutterVersion: () => '[user-branch]/v1.2.3',
        command: 'crash',
      );

      // Verify that the report wasn't sent
      expect(fakeHttpClient.lastUrl, null);

      expect(logger.traceText, isNot(contains('Crash report sent')));
    });

    testWithoutContext('can override base URL', () async {
      final FakeHttpClient fakeHttpClient = FakeHttpClient();

      final Platform environmentPlatform = FakePlatform(
        operatingSystem: 'linux',
        environment: <String, String>{
          'HOME': '/',
          'FLUTTER_CRASH_SERVER_BASE_URL': 'https://localhost:12345/fake_server',
        },
        script: Uri(scheme: 'data'),
      );

      final CrashReportSender crashReportSender = CrashReportSender(
        client: fakeHttpClient,
        usage: mockUsage,
        platform: environmentPlatform,
        logger: logger,
        operatingSystemUtils: operatingSystemUtils,
      );

      await crashReportSender.sendReport(
        error: StateError('Test bad state error'),
        stackTrace: null,
        getFlutterVersion: () => 'test-version',
        command: 'crash',
      );

      // Verify that we sent the crash report.
      expect(fakeHttpClient.lastUrl, isNotNull);
      expect(fakeHttpClient.lastUrl, Uri(
        scheme: 'https',
        host: 'localhost',
        port: 12345,
        path: '/fake_server',
        queryParameters: <String, String>{
          'product': 'Flutter_Tools',
          'version': 'test-version',
        },
      ));
    });
  });
}

class FakeHttpClient extends Fake implements HttpClient {
  FakeHttpClient([this.exception]);

  dynamic exception;
  int calls = 0;
  Uri lastUrl;
  final FakeHttpClientRequest httpClientRequest = FakeHttpClientRequest();

  @override
  Future<HttpClientRequest> postUrl(Uri url) async {
    calls += 1;
    lastUrl = url;
    if (exception != null) {
      throw exception;
    }
    return httpClientRequest;
  }
}

class FakeHttpClientRequest extends Fake implements HttpClientRequest {
  final FakeHttpClientResponse response = FakeHttpClientResponse();
  final List<int> chunks = <int>[];

  @override
  final HttpHeaders headers = FakeHttpHeaders();

  @override
  Future<HttpClientResponse> close() async {
    return response;
  }

  @override
  void add(List<int> data) {
    chunks.addAll(data);
  }
}

class FakeHttpClientResponse extends Fake implements HttpClientResponse {
  @override
  int statusCode = HttpStatus.ok;

  @override
  Stream<S> transform<S>(StreamTransformer<List<int>, S> streamTransformer) {
    return streamTransformer.bind(const Stream<List<int>>.empty());
  }
}

class FakeHttpHeaders extends Fake implements HttpHeaders {
  final Map<String, List<String>> headerValues = <String, List<String>>{};
  
  @override
  void add(String name, Object value, {bool preserveHeaderCase = false}) {
    headerValues[name] ??= <String>[];
    headerValues[name].add(value.toString());
  }

  @override
  void set(String name, Object value, {bool preserveHeaderCase = false}) {
    headerValues[name] = <String>[value.toString()];
  }
}

/// A DoctorValidatorsProvider that overrides the default validators without
/// overriding the doctor.
class FakeDoctorValidatorsProvider implements DoctorValidatorsProvider {
  @override
  List<DoctorValidator> get validators => <DoctorValidator>[];

  @override
  List<Workflow> get workflows => <Workflow>[];
}

class MockUsage extends Mock implements Usage {}
