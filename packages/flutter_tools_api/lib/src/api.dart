// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';

import 'config.dart';

/// A screen that can run a flutter application.
///
/// The device encapsulates the functionality necessary to run, install, quit
/// and diagnose issues with the hardware running a flutter application.
///
/// A device may represent a physical device, such as a connected Android
/// phone, or an ephemeral device such as a tester shell.
abstract class Device {
  /// The returned uri must represent an address on localhost, and not
  /// a remote address. The connected device is responsible for port forwarding
  /// to the host address.
  Future<Uri> launch(Session session);

  Future<void> stop(Session session);

  Future<TargetPlatform> get targetPlatform;

  Future<DeviceConfiguration> get deviceConfiguration;

  /// A value which uniquely identifies a device within an extension.
  ///
  /// This value is not required to be globally unique.
  String get id;
}

/// A token which represents the current resident process.
///
/// This can be used to store configurations by type.
class Session {
  Session._();

  final Map<Type, Object> _storage = <Type, Object>{};

  /// Save the `value` of type `T` in session storage.
  void save<T>(T value) => _storage[T] = value;

  /// Retrieve the value corresponding to type `T` in session storage.
  T retrieve<T>() => _storage[T] as T;
}

/// Many extensions will require additional programs to be installed and
/// accessible in order to work. The doctor provides APIs to simplify the
/// process and communicate fixes to the user.
abstract class Doctor {

}

/// A process for producing artifacts for device consumption.
///
/// One or more [Device] instances may share a single workflow and the
/// corresponding artifacts. This is configured in the top-level
/// [Extension] api.
abstract class Workflow {
  /// Produce the required platform artifacts.
  Future<void> build(BuildRequest buildRequest);
}

class DeviceConfiguration {
  DeviceConfiguration(
    this.supportedPerformanceModes,
    this.supportedCompileModes,
  );

  final Set<PerformanceMode> supportedPerformanceModes;
  final Set<CompileMode> supportedCompileModes;
}

/// An extension is the hook into the flutter_tool process.
abstract class Extension {
  Future<List<Device>> listDevices();

  /// The workflow shared across all device types returned by this extension.
  Workflow get workflow;

  /// The doctor instance of this device type.
  Doctor get doctor;
}

