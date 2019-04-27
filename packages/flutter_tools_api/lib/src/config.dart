import 'dart:io';

class HostPlatform {
  const HostPlatform._(this.value);

  static const HostPlatform windows_x64 = HostPlatform._(0);
  static const HostPlatform linux_x64 = HostPlatform._(1);
  static const HostPlatform darwin_x64 = HostPlatform._(2);

  final int value;
}

class TargetPlatform {
  const TargetPlatform._(this.value);

  static const TargetPlatform android_arm = TargetPlatform._(0);
  static const TargetPlatform android_arm64 = TargetPlatform._(1);
  static const TargetPlatform android_x64 = TargetPlatform._(2);
  static const TargetPlatform android_x86 = TargetPlatform._(3);
  static const TargetPlatform ios_armv7 = TargetPlatform._(4);
  static const TargetPlatform ios_arm64 = TargetPlatform._(5);
  static const TargetPlatform darwin_x64 = TargetPlatform._(6);
  static const TargetPlatform linux_x64 = TargetPlatform._(7);
  static const TargetPlatform windows_x64 = TargetPlatform._(8);
  static const TargetPlatform fuchsia = TargetPlatform._(9);
  static const TargetPlatform tester = TargetPlatform._(10);
  static const TargetPlatform web = TargetPlatform._(11);

  final int value;
}

class PerformanceMode {
  const PerformanceMode._(this.value);

  static const PerformanceMode debug = PerformanceMode._(0);

  static const PerformanceMode release = PerformanceMode._(1);

  static const PerformanceMode profile = PerformanceMode._(2);

  final int value;
}

/// The type of Dart compilation.
class CompileMode {
  const CompileMode._(this.value);

  /// JIT compilation mode.
  ///
  /// This requires a full Dart VM in order to run the flutter application.
  static const CompileMode jit = CompileMode._(0);

  /// AOT compilation mode.
  ///
  /// This requires a Dart runtime to load the flutter library.
  static const CompileMode aot = CompileMode._(1);

  /// JavaScript compilation mode.
  ///
  /// This requires a JavaScript runtime to load the flutter script.
  static const CompileMode javaScript = CompileMode._(2);

  final int value;
}

/// A request to build the platform specific artifacts.
class BuildRequest {
  const BuildRequest._(
    this.cacheDirectory,
    this.platformDirectory,
    this.applicationDirectory,
    this.performanceMode,
    this.compileMode,
    this.trackWidgetCreation,
  );

  /// Whether the track widget creation flag is enabled.
  final bool trackWidgetCreation;

  final PerformanceMode performanceMode;

  final CompileMode compileMode;

  /// The location where build artifacts should be written to.
  final Directory cacheDirectory;

  /// The location where platform specific artifacts can be read from and
  /// written to.
  final Directory platformDirectory;

  /// The location of the Flutter application root.
  final Directory applicationDirectory;
}
