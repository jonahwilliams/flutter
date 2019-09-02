// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';

import 'package:meta/meta.dart';

import 'base/file_system.dart';
import 'device.dart';
import 'globals.dart';
import 'resident_runner.dart';
import 'tracing.dart';
import 'vmservice.dart';

// TODO(mklim): Test this, flutter/flutter#23031.
class ColdRunner extends ResidentRunner {
  ColdRunner(
    FlutterDevice flutterDevice, {
    String target,
    DebuggingOptions debuggingOptions,
    this.traceStartup = false,
    this.awaitFirstFrameWhenTracing = true,
    this.applicationBinary,
    bool ipv6 = false,
    bool stayResident = true,
  }) : super(flutterDevice,
             target: target,
             debuggingOptions: debuggingOptions,
             hotMode: false,
             stayResident: stayResident,
             ipv6: ipv6);

  final bool traceStartup;
  final bool awaitFirstFrameWhenTracing;
  final File applicationBinary;
  bool _didAttach = false;

  @override
  bool get canHotReload => false;

  @override
  bool get canHotRestart => false;

  @override
  Future<int> run({
    Completer<DebugConnectionInfo> connectionInfoCompleter,
    void Function() onAppStarted,
    String route,
  }) async {
    final bool prebuiltMode = applicationBinary != null;
    if (!prebuiltMode) {
      if (!fs.isFileSync(mainPath)) {
        String message = 'Tried to run $mainPath, but that file does not exist.';
        if (target == null)
          message += '\nConsider using the -t option to specify the Dart file to start.';
        printError(message);
        return 1;
      }
    }

    final int result = await flutterDevice.runCold(
      coldRunner: this,
      route: route,
    );
    if (result != 0) {
      return result;
    }

    // Connect to observatory.
    if (debuggingOptions.debuggingEnabled) {
      try {
        await connectToServiceProtocol();
      } on String catch (message) {
        printError(message);
        return 2;
      }
    }

    if (flutterDevice.observatoryUris != null) {
      // For now, only support one debugger connection.
      connectionInfoCompleter?.complete(DebugConnectionInfo(
        httpUri: flutterDevice.observatoryUris.first,
        wsUri: flutterDevice.vmServices.first.wsAddress,
      ));
    }

    printTrace('Application running.');

    if (flutterDevice.vmServices != null) {
      flutterDevice.initLogReader();
      await flutterDevice.refreshViews();
      printTrace('Connected to ${flutterDevice.device.name}');
    }

    if (traceStartup) {
      // Only trace startup for the first device.
      if (flutterDevice.vmServices != null && flutterDevice.vmServices.isNotEmpty) {
        printStatus('Tracing startup on ${flutterDevice.device.name}.');
        await downloadStartupTrace(
          flutterDevice.vmServices.first,
          awaitFirstFrame: awaitFirstFrameWhenTracing,
        );
      }
      appFinished();
    }

    if (onAppStarted != null) {
      onAppStarted();
    }

    if (stayResident && !traceStartup)
      return waitForAppToFinish();
    await cleanupAtFinish();
    return 0;
  }

  @override
  Future<int> attach({
    Completer<DebugConnectionInfo> connectionInfoCompleter,
    void Function() onAppStarted,
  }) async {
    _didAttach = true;
    try {
      await connectToServiceProtocol();
    } catch (error) {
      printError('Error connecting to the service protocol: $error');
      // https://github.com/flutter/flutter/issues/33050
      // TODO(blasten): Remove this check once https://issuetracker.google.com/issues/132325318 has been fixed.
      if (await hasDeviceRunningAndroidQ(flutterDevice) &&
          error.toString().contains(kAndroidQHttpConnectionClosedExp)) {
        printStatus('🔨 If you are using an emulator running Android Q Beta, consider using an emulator running API level 29 or lower.');
        printStatus('Learn more about the status of this issue on https://issuetracker.google.com/issues/132325318');
      }
      return 2;
    }
    flutterDevice.initLogReader();
    await refreshViews();
    for (FlutterView view in flutterDevice.views) {
      printTrace('Connected to $view.');
    }
    if (onAppStarted != null) {
      onAppStarted();
    }
    if (stayResident) {
      return waitForAppToFinish();
    }
    await cleanupAtFinish();
    return 0;
  }

  @override
  Future<void> cleanupAfterSignal() async {
    await stopEchoingDeviceLog();
    if (_didAttach) {
      appFinished();
    }
    await exitApp();
  }

  @override
  Future<void> cleanupAtFinish() async {
    await stopEchoingDeviceLog();
  }

  @override
  void printHelp({ @required bool details }) {
    bool haveDetails = false;
    bool haveAnything = false;
    final String dname = flutterDevice.device.name;
    if (flutterDevice.observatoryUris != null) {
      for (Uri uri in flutterDevice.observatoryUris) {
        printStatus('An Observatory debugger and profiler on $dname is available at $uri');
        haveAnything = true;
      }
    }
    if (supportsServiceProtocol) {
      haveDetails = true;
      if (details) {
        printHelpDetails();
        haveAnything = true;
      }
    }
    final String quitMessage = _didAttach
      ? 'To detach, press "d"; to quit, press "q".'
      : 'To quit, press "q".';
    if (haveDetails && !details) {
      printStatus('For a more detailed help message, press "h". $quitMessage');
    } else if (haveAnything) {
      printStatus('To repeat this help message, press "h". $quitMessage');
    } else {
      printStatus(quitMessage);
    }
  }

  @override
  Future<void> preExit() async {
    // If we're running in release mode, stop the app using the device logic.
    if (flutterDevice.vmServices == null || flutterDevice.vmServices.isEmpty) {
      await flutterDevice.device.stopApp(flutterDevice.package);
    }
  }
}
