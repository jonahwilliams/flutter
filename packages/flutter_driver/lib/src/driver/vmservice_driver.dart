// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:file/file.dart' as f;
import 'package:fuchsia_remote_debug_protocol/fuchsia_remote_debug_protocol.dart' as fuchsia;
import 'package:meta/meta.dart';
import 'package:path/path.dart' as p;
import 'package:vm_service/vm_service.dart' hide Timeline;
import 'package:vm_service/vm_service_io.dart';

import '../../flutter_driver.dart';
import '../common/error.dart';
import '../common/frame_sync.dart';
import '../common/fuchsia_compat.dart';
import '../common/health.dart';
import '../common/message.dart';
import 'common.dart';
import 'driver.dart';
import 'timeline.dart';

const String _flutterExtensionMethodName = 'ext.flutter.driver';

/// An implementation of the Flutter Driver over the vmservice protocol.
class VMServiceFlutterDriver extends FlutterDriver {
  /// Creates a driver that uses a connection provided by the given
  /// [serviceClient], [_peer] and [appIsolate].
  VMServiceFlutterDriver.connectedTo(
      this._serviceClient,
      this._appIsolate, {
        bool printCommunication = false,
        bool logCommunicationToFile = true,
      }) : _printCommunication = printCommunication,
        _logCommunicationToFile = logCommunicationToFile,
        _driverId = _nextDriverId++;

  /// Connects to a Flutter application.
  ///
  /// See [FlutterDriver.connect] for more documentation.
  static Future<FlutterDriver> connect({
    String dartVmServiceUrl,
    bool printCommunication = false,
    bool logCommunicationToFile = true,
    int isolateNumber,
    Pattern fuchsiaModuleTarget,
    Map<String, dynamic> headers,
  }) async {
    // If running on a Fuchsia device, connect to the first isolate whose name
    // matches FUCHSIA_MODULE_TARGET.
    //
    // If the user has already supplied an isolate number/URL to the Dart VM
    // service, then this won't be run as it is unnecessary.
    if (Platform.isFuchsia && isolateNumber == null) {
      // TODO(awdavies): Use something other than print. On fuchsia
      // `stderr`/`stdout` appear to have issues working correctly.
      driverLog = (String source, String message) {
        print('$source: $message');
      };
      fuchsiaModuleTarget ??= Platform.environment['FUCHSIA_MODULE_TARGET'];
      if (fuchsiaModuleTarget == null) {
        throw DriverError(
            'No Fuchsia module target has been specified.\n'
                'Please make sure to specify the FUCHSIA_MODULE_TARGET '
                'environment variable.'
        );
      }
      final fuchsia.FuchsiaRemoteConnection fuchsiaConnection =
      await FuchsiaCompat.connect();
      final List<fuchsia.IsolateRef> refs =
      await fuchsiaConnection.getMainIsolatesByPattern(fuchsiaModuleTarget);
      final fuchsia.IsolateRef ref = refs.first;
      isolateNumber = ref.number;
      dartVmServiceUrl = ref.dartVm.uri.toString();
      await fuchsiaConnection.stop();
      FuchsiaCompat.cleanup();
    }

    dartVmServiceUrl ??= Platform.environment['VM_SERVICE_URL'];

    if (dartVmServiceUrl == null) {
      throw DriverError(
          'Could not determine URL to connect to application.\n'
              'Either the VM_SERVICE_URL environment variable should be set, or an explicit '
              'URL should be provided to the FlutterDriver.connect() method.'
      );
    }

    // Connect to Dart VM services
    _log('Connecting to Flutter application at $dartVmServiceUrl');
    final VmServiceConnection connection =
    await vmServiceConnectFunction(dartVmServiceUrl, headers: headers);
    final VmService client = connection.client;
    final VM vm = await client.getVM();
    final IsolateRef isolateRef = isolateNumber == null ? vm.isolates.first :
    vm.isolates.firstWhere((IsolateRef isolate) => isolate.number == isolateNumber.toString());
    _log('Isolate found with number: ${isolateRef.number}');

    final Isolate isolate = await client.getIsolate(isolateRef.id);
    final VMServiceFlutterDriver driver = VMServiceFlutterDriver.connectedTo(
      client,
      isolate,
      printCommunication: printCommunication,
      logCommunicationToFile: logCommunicationToFile,
    );

    // Attempts to resume the isolate, but does not crash if it fails because
    // the isolate is already resumed. There could be a race with other tools,
    // such as a debugger, any of which could have resumed the isolate.
    Future<dynamic> resumeLeniently() {
      _log('Attempting to resume isolate');
      return client.resume(isolate.id).catchError((dynamic e) {
        const int vmMustBePausedCode = 101;
        if (e is RPCError && e.code == vmMustBePausedCode) {
          // No biggie; something else must have resumed the isolate
          _log(
            'Attempted to resume an already resumed isolate. This may happen '
            'when we lose a race with another tool (usually a debugger) that '
            'is connected to the same isolate.'
          );
        } else {
          // Failed to resume due to another reason. Fail hard.
          throw e;
        }
      });
    }

    /// Waits for a signal from the VM service that the extension is registered.
    ///
    /// Looks at the list of loaded extensions for the current [isolateRef], as
    /// well as the stream of added extensions.
    Future<void> waitForServiceExtension() async {
      final Completer<void> extensionAdded = Completer<void>();
      StreamSubscription<Event> isolateAddedSubscription;
      isolateAddedSubscription = client.onExtensionEvent.listen(
        (Event event) {
          print(event.json);
          // if (event.extensionName == _flutterExtensionMethodName) {
            extensionAdded.complete();
            isolateAddedSubscription.cancel();
          //}
        },
        onError: extensionAdded.completeError,
        cancelOnError: true,
      );
      await client.streamListen(EventStreams.kExtension);
      final Isolate isolate = await client.getIsolate(isolateRef.id);
      if (isolate.extensionRPCs.contains(_flutterExtensionMethodName)) {
        isolateAddedSubscription.cancel();
        return;
      }

      await extensionAdded.future;
    }

    await client.streamListen(EventStreams.kIsolate);

    // Attempt to resume isolate if it was paused
    if (isPauseEvent(isolate.pauseEvent.kind)) {
      await resumeLeniently();
    }

    // driver will never receive the extension event if the user does not register
    // it. If that happens, show a message but continue waiting.
    await _warnIfSlow<void>(
      future: waitForServiceExtension(),
      timeout: kUnusuallyLongTimeout,
      message: 'Flutter Driver extension is taking a long time to become available. '
          'Ensure your test app (often "lib/main.dart") imports '
          '"package:flutter_driver/driver_extension.dart" and '
          'calls enableFlutterDriverExtension() as the first call in main().',
    );

    final Health health = await driver.checkHealth();
    if (health.status != HealthStatus.ok) {
      client.dispose();
      throw DriverError('Flutter application health check failed.');
    }

    _log('Connected to Flutter application.');
    return driver;
  }

  static int _nextDriverId = 0;

  // The additional blank line in the beginning is for _log.
  static const String _kDebugWarning = '''
┏╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍┓
┇ ⚠    THIS BENCHMARK IS BEING RUN IN DEBUG MODE     ⚠  ┇
┡╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍╍┦
│                                                       │
│  Numbers obtained from a benchmark while asserts are  │
│  enabled will not accurately reflect the performance  │
│  that will be experienced by end users using release  ╎
│  builds. Benchmarks should be run using this command  ┆
│  line:  flutter drive --profile test_perf.dart        ┊
│                                                       ┊
└─────────────────────────────────────────────────╌┄┈  🐢
''';
  /// The unique ID of this driver instance.
  final int _driverId;

  /// Client connected to the Dart VM running the Flutter application
  ///
  /// You can use [VmService] to check VM version, flags and get
  /// notified when a new isolate has been instantiated. That could be
  /// useful if your application spawns multiple isolates that you
  /// would like to instrument.
  final VmService _serviceClient;

  @override
  Isolate get appIsolate => _appIsolate;

  @override
  VmService get serviceClient => _serviceClient;

  /// The main isolate hosting the Flutter application.
  ///
  /// If you used the [registerExtension] API to instrument your application,
  /// you can use this [Isolate] to call these extension methods via
  /// [invokeExtension].
  final Isolate _appIsolate;

  /// Whether to print communication between host and app to `stdout`.
  final bool _printCommunication;

  /// Whether to log communication between host and app to `flutter_driver_commands.log`.
  final bool _logCommunicationToFile;

  @override
  Future<Map<String, dynamic>> sendCommand(Command command) async {
    Response response;
    try {
      final Map<String, String> serialized = command.serialize();
      _logCommunication('>>> $serialized');
      final Future<Response> reponse = _serviceClient.callServiceExtension(
        _flutterExtensionMethodName,
        isolateId: _appIsolate.id,
        args: serialized,
      );
      response = await _warnIfSlow<Response>(
        future: reponse,
        timeout: command.timeout ?? kUnusuallyLongTimeout,
        message: '${command.kind} message is taking a long time to complete...',
      );
      _logCommunication('<<< $response');
    } catch (error, stackTrace) {
      throw DriverError(
        'Failed to fulfill ${command.runtimeType} due to remote error',
        error,
        stackTrace,
      );
    }
    if (response.json['isError'] as bool)
      throw DriverError('Error in Flutter application: ${response.json['response']}');
    return response.json['response'] as Map<String, dynamic>;
  }

  void _logCommunication(String message) {
    if (_printCommunication)
      _log(message);
    if (_logCommunicationToFile) {
      final f.File file = fs.file(p.join(testOutputsDirectory, 'flutter_driver_commands_$_driverId.log'));
      file.createSync(recursive: true); // no-op if file exists
      file.writeAsStringSync('${DateTime.now()} $message\n', mode: f.FileMode.append, flush: true);
    }
  }

  @override
  Future<List<int>> screenshot() async {
    await Future<void>.delayed(const Duration(seconds: 2));

    final Map<String, dynamic> result = (await serviceClient.callServiceExtension(
      '_flutter.screenshot',
      isolateId: _appIsolate.id,
    )).json;
    return base64.decode(result['screenshot'] as String);
  }

  @override
  Future<List<Flag>> getVmFlags() async {
    final FlagList result = await serviceClient.getFlagList();
    return result.flags;
  }

  Future<Timestamp> _getVMTimelineMicros() async {
    return await serviceClient.getVMTimelineMicros();
  }

  @override
  Future<void> startTracing({
    List<TimelineStream> streams = const <TimelineStream>[TimelineStream.all],
    Duration timeout = kUnusuallyLongTimeout,
  }) async {
    assert(streams != null && streams.isNotEmpty);
    assert(timeout != null);
    try {
      await _warnIfSlow<void>(
        future: serviceClient.setVMTimelineFlags(_timelineStreamsToString(streams).toList()),
        timeout: timeout,
        message: 'VM is taking an unusually long time to respond to being told to start tracing...',
      );
    } catch (error, stackTrace) {
      throw DriverError(
        'Failed to start tracing due to remote error',
        error,
        stackTrace,
      );
    }
  }

  @override
  Future<Timeline> stopTracingAndDownloadTimeline({
    Duration timeout = kUnusuallyLongTimeout,
    int startTime,
    int endTime,
  }) async {
    assert(timeout != null);
    assert((startTime == null && endTime == null) ||
           (startTime != null && endTime != null));

    try {
      await _warnIfSlow<void>(
        future: _serviceClient.setVMTimelineFlags(<String>[]),
        timeout: timeout,
        message: 'VM is taking an unusually long time to respond to being told to stop tracing...',
      );
      if (startTime == null) {
        return Timeline.fromJson((await _serviceClient.getVMTimeline()).json);
      }
      const int kSecondInMicros = 1000000;
      int currentStart = startTime;
      int currentEnd = startTime + kSecondInMicros; // 1 second of timeline
      final List<Map<String, Object>> chunks = <Map<String, Object>>[];
      do {
        final Map<String, Object> chunk = (await _serviceClient.getVMTimeline(
          timeOriginMicros:  currentStart,
          // The range is inclusive, avoid double counting on the chance something
          // aligns on the boundary.
          timeExtentMicros: kSecondInMicros - 1,
        )).json;
        chunks.add(chunk);
        currentStart = currentEnd;
        currentEnd += kSecondInMicros;
      } while (currentStart < endTime);
      return Timeline.fromJson(<String, Object>{
        'traceEvents': <Object> [
          for (Map<String, Object> chunk in chunks)
            ...chunk['traceEvents'] as List<Object>,
        ],
      });
    } catch (error, stackTrace) {
      throw DriverError(
        'Failed to stop tracing due to remote error',
        error,
        stackTrace,
      );
    }
  }

  Future<bool> _isPrecompiledMode() async {
    final List<Flag> flags = await getVmFlags();
    for (final Flag flag in flags) {
      if (flag.name == 'precompiled_mode') {
        return flag.valueAsString == 'true';
      }
    }
    return false;
  }

  @override
  Future<Timeline> traceAction(
      Future<dynamic> action(), {
        List<TimelineStream> streams = const <TimelineStream>[TimelineStream.all],
        bool retainPriorEvents = false,
      }) async {
    if (!retainPriorEvents) {
      await clearTimeline();
    }
    final Timestamp startTimestamp = await _getVMTimelineMicros();
    await startTracing(streams: streams);
    await action();
    final Timestamp endTimestamp = await _getVMTimelineMicros();

    if (!(await _isPrecompiledMode())) {
      _log(_kDebugWarning);
    }

    return stopTracingAndDownloadTimeline(
      startTime: startTimestamp.timestamp,
      endTime: endTimestamp.timestamp,
    );
  }

  @override
  Future<void> clearTimeline({
    Duration timeout = kUnusuallyLongTimeout,
  }) async {
    assert(timeout != null);
    try {
      await _warnIfSlow<void>(
        future: _serviceClient.clearVMTimeline(),
        timeout: timeout,
        message: 'VM is taking an unusually long time to respond to being told to clear its timeline buffer...',
      );
    } catch (error, stackTrace) {
      throw DriverError(
        'Failed to clear event timeline due to remote error',
        error,
        stackTrace,
      );
    }
  }

  @override
  Future<T> runUnsynchronized<T>(Future<T> action(), { Duration timeout }) async {
    await sendCommand(SetFrameSync(false, timeout: timeout));
    T result;
    try {
      result = await action();
    } finally {
      await sendCommand(SetFrameSync(true, timeout: timeout));
    }
    return result;
  }

  @override
  Future<void> forceGC() async { }

  @override
  Future<void> close() async {
    _serviceClient.dispose();
  }
}


/// The connection function used by [FlutterDriver.connect].
///
/// Overwrite this function if you require a custom method for connecting to
/// the VM service.
VMServiceConnectFunction vmServiceConnectFunction = _waitAndConnect;

/// Restores [vmServiceConnectFunction] to its default value.
void restoreVmServiceConnectFunction() {
  vmServiceConnectFunction = _waitAndConnect;
}


String _getWebSocketUrl(String url) {
  Uri uri = Uri.parse(url);
  final List<String> pathSegments = <String>[
    // If there's an authentication code (default), we need to add it to our path.
    if (uri.pathSegments.isNotEmpty) uri.pathSegments.first,
    'ws',
  ];
  if (uri.scheme == 'http')
    uri = uri.replace(scheme: 'ws', pathSegments: pathSegments);
  return uri.toString();
}

/// Waits for a real Dart VM service to become available, then connects using
/// the [VmService].
Future<VmServiceConnection> _waitAndConnect(
    String url, {Map<String, dynamic> headers}) async {
  final String webSocketUrl = _getWebSocketUrl(url);
  int attempts = 0;
  while (true) {
    WebSocket ws1;
    try {
      return VmServiceConnection(await vmServiceConnectUri(webSocketUrl));
    } catch (e) {
      await ws1?.close();
      if (attempts > 5)
        _log('It is taking an unusually long time to connect to the VM...');
      attempts += 1;
      await Future<void>.delayed(_kPauseBetweenReconnectAttempts);
    }
  }
}


/// The amount of time we wait prior to making the next attempt to connect to
/// the VM service.
const Duration _kPauseBetweenReconnectAttempts = Duration(seconds: 1);

// See https://github.com/dart-lang/sdk/blob/master/runtime/vm/timeline.cc#L32
Iterable<String> _timelineStreamsToString(List<TimelineStream> streams) {
  return streams.map<String>((TimelineStream stream) {
    switch (stream) {
      case TimelineStream.all: return 'all';
      case TimelineStream.api: return 'API';
      case TimelineStream.compiler: return 'Compiler';
      case TimelineStream.dart: return 'Dart';
      case TimelineStream.debugger: return 'Debugger';
      case TimelineStream.embedder: return 'Embedder';
      case TimelineStream.gc: return 'GC';
      case TimelineStream.isolate: return 'Isolate';
      case TimelineStream.vm: return 'VM';
      default:
        throw 'Unknown timeline stream $stream';
    }
  });
}

void _log(String message) {
  driverLog('VMServiceFlutterDriver', message);
}
Future<T> _warnIfSlow<T>({
  @required Future<T> future,
  @required Duration timeout,
  @required String message,
}) {
  assert(future != null);
  assert(timeout != null);
  assert(message != null);
  future
    .timeout(timeout, onTimeout: () {
      _log(message);
      return null;
    })
    // Don't duplicate errors if [future] completes with an error.
    .catchError((dynamic e) => null);

  return future;
}

/// Encapsulates connection information to an instance of a Flutter application.
@visibleForTesting
class VmServiceConnection {
  /// Creates an instance of this class given a [client] and a [peer].
  VmServiceConnection(this.client);

  /// Use this for structured access to the VM service's public APIs.
  final VmService client;
}

/// A function that connects to a Dart VM service
/// with [headers] given the [url].
typedef VMServiceConnectFunction =
  Future<VmServiceConnection> Function(
    String url, {Map<String, dynamic> headers});

/// Whether the event attached to an [Isolate.pauseEvent] should be considered
/// a "pause" event.
bool isPauseEvent(String kind) {
  return kind == EventKind.kPauseStart ||
         kind == EventKind.kPauseExit ||
         kind == EventKind.kPauseBreakpoint ||
         kind == EventKind.kPauseInterrupted ||
         kind == EventKind.kPauseException ||
         kind == EventKind.kPausePostRequest ||
         kind == EventKind.kNone;
}
