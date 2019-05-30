

import 'package:meta/meta.dart';

/// An extension allows adding new core functionality to the flutter tool.
abstract class Extension {

  /// The name of the extension, used to identify it in tooling and IDEs.
  String get name;

  /// A version string for the plugin instance.
  ///
  /// This should be a valid semver String such as `1.0.0` or `0.4.3`.
  String get version;

  /// An implementation of the [DevicesDomain].
  ///
  /// if no additional functionality is provided by this extension, it can be
  /// left as null.
  DevicesDomain get devicesDomain;
}

/// A domain contains a specific vertical of tool functionality.
///
/// It is not required for an extension to provide an implementation for every
/// domain, but if a domain is provided then every method within it requires an
/// implementation.
abstract class Domain {

  /// The name of the domain.
  @protected
  String get name;
}

abstract class DevicesDomain implements Domain {
  @override
  String get name => 'devices';


  Future<int> forwardPort(int devicePort, int hostPort);

  Future<void> unforwardPort(int port);
}