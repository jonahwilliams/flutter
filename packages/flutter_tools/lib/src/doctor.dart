// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


import 'package:meta/meta.dart';
import 'package:process/process.dart';

import 'android/android_studio_validator.dart';
import 'android/android_workflow.dart';
import 'artifacts.dart';
import 'base/async_guard.dart';
import 'base/config.dart';
import 'base/file_system.dart';
import 'base/logger.dart';
import 'base/platform.dart';
import 'base/terminal.dart';
import 'base/user_messages.dart';
import 'base/utils.dart';
import 'cache.dart';
import 'device.dart';
import 'features.dart';
import 'fuchsia/fuchsia_workflow.dart';
import 'globals.dart' as globals;
import 'intellij/intellij_validator.dart';
import 'ios/ios_workflow.dart';
import 'linux/linux_doctor.dart';
import 'linux/linux_workflow.dart';
import 'macos/cocoapods_validator.dart';
import 'macos/macos_workflow.dart';
import 'macos/xcode_validator.dart';
import 'proxy_validator.dart';
import 'reporting/reporting.dart';
import 'tester/flutter_tester.dart';
import 'version.dart';
import 'vscode/vscode_validator.dart';
import 'web/chrome.dart';
import 'web/web_validator.dart';
import 'web/workflow.dart';
import 'windows/visual_studio_validator.dart';
import 'windows/windows_workflow.dart';

/// An injection interface for the validators given by flutter doctor.
///
/// This may be overriden to provide different validators based on
/// the environment.
abstract class DoctorValidatorsProvider {
  /// The current set of validators supported by the tool.
  ///
  /// See also:
  ///   * [DoctorValidator], the base type these derive from.
  ///   * [LinuxDoctorValidator], for an example of an implementation for Linux desktop
  ///     development.
  List<DoctorValidator> get validators;

  /// The current set of workflows supported by the tool.
  ///
  /// See also:
  ///   * [Workflow], the base type these derive from.
  ///   * [AndroidWorkflow], for an example of an implementation for Android development.
  List<Workflow> get workflows;
}

/// An implementation of the doctor validator provider used by
/// default in the flutter tool.
class FlutterDoctorValidatorsProvider implements DoctorValidatorsProvider {
  FlutterDoctorValidatorsProvider({
    @required Platform platform,
    @required Config config,
    @required UserMessages userMessages,
    @required FileSystem fileSystem,
    @required DeviceManager deviceManager,
    @required FlutterVersion Function() flutterVersion,
    @required LinuxWorkflow linuxWorkflow,
    @required WebWorkflow webWorkflow,
    @required MacOSWorkflow macOSWorkflow,
    @required WindowsWorkflow windowsWorkflow,
    @required FuchsiaWorkflow fuchsiaWorkflow,
    @required AndroidWorkflow androidWorkflow,
    @required IOSWorkflow iosWorkflow,
  }) : _platform = platform,
       _config = config,
       _userMessages = userMessages,
       _fileSystem = fileSystem,
       _deviceManager = deviceManager,
       _linuxWorkflow = linuxWorkflow,
       _webWorkflow = webWorkflow,
       _macOSWorkflow = macOSWorkflow,
       _windowsWorkflow = windowsWorkflow,
       _fuchsiaWorkflow = fuchsiaWorkflow,
       _androidWorkflow = androidWorkflow,
       _iosWorkflow = iosWorkflow,
       _flutterVersion = flutterVersion;

  // All supported workflows.
  final LinuxWorkflow _linuxWorkflow;
  final WebWorkflow _webWorkflow;
  final MacOSWorkflow _macOSWorkflow;
  final WindowsWorkflow _windowsWorkflow;
  final FuchsiaWorkflow _fuchsiaWorkflow;
  final AndroidWorkflow _androidWorkflow;
  final IOSWorkflow _iosWorkflow;

  final Platform _platform;
  final Config _config;
  final UserMessages _userMessages;
  final FileSystem _fileSystem;
  final DeviceManager _deviceManager;
  final FlutterVersion Function() _flutterVersion;

  List<DoctorValidator> _validators;
  List<Workflow> _workflows;

  @override
  List<DoctorValidator> get validators {
    if (_validators != null) {
      return _validators;
    }

    final List<DoctorValidator> ideValidators = <DoctorValidator>[
      ...AndroidStudioValidator.allValidators(_config, _platform, _fileSystem, _userMessages),
      ...IntelliJValidator.installedValidators(_fileSystem, _platform),
      ...VsCodeValidator.installedValidators(_fileSystem, _platform),
    ];
    final ProxyValidator proxyValidator = ProxyValidator(platform: _platform);
    _validators = <DoctorValidator>[
      FlutterValidator(
        platform: _platform,
        userMessages: _userMessages,
        flutterVersion: _flutterVersion,
        fileSystem: _fileSystem,
        artifacts: globals.artifacts,
        processManager: globals.processManager,
      ),
      if (androidWorkflow.appliesToHostPlatform)
        GroupedValidator(<DoctorValidator>[androidValidator, androidLicenseValidator]),
      if (_iosWorkflow.appliesToHostPlatform || _macOSWorkflow.appliesToHostPlatform)
        GroupedValidator(<DoctorValidator>[
          XcodeValidator(xcode: globals.xcode, userMessages: userMessages),
          cocoapodsValidator,
        ]),
      if (_webWorkflow.appliesToHostPlatform)
        ChromeValidator(
          chromiumLauncher: ChromiumLauncher(
            browserFinder: findChromeExecutable,
            fileSystem: _fileSystem,
            operatingSystemUtils: globals.os,
            platform:  _platform,
            processManager: globals.processManager,
            logger: globals.logger,
          ),
          platform: _platform,
        ),
      if (_linuxWorkflow.appliesToHostPlatform)
        LinuxDoctorValidator(
          processManager: globals.processManager,
          userMessages: userMessages,
        ),
      if (_windowsWorkflow.appliesToHostPlatform)
        visualStudioValidator,
      if (ideValidators.isNotEmpty)
        ...ideValidators
      else
        NoIdeValidator(userMessages: _userMessages),
      if (proxyValidator.shouldShow)
        proxyValidator,
      if (_deviceManager.canListAnything)
        DeviceValidator(
          deviceManager: _deviceManager,
          userMessages: _userMessages,
        ),
    ];
    return _validators;
  }

  @override
  List<Workflow> get workflows {
    if (_workflows != null) {
      return _workflows;
    }
    _workflows = <Workflow>[
      if (_androidWorkflow.appliesToHostPlatform)
        _androidWorkflow,
      if (_iosWorkflow.appliesToHostPlatform)
        _iosWorkflow,
      if (_linuxWorkflow.appliesToHostPlatform)
        _linuxWorkflow,
      if (_macOSWorkflow.appliesToHostPlatform)
        _macOSWorkflow,
      if (_windowsWorkflow.appliesToHostPlatform)
        _windowsWorkflow,
      if (_webWorkflow.appliesToHostPlatform)
        _webWorkflow,
      if (_fuchsiaWorkflow.appliesToHostPlatform)
        _fuchsiaWorkflow,
    ];
    return _workflows;
  }
}

class ValidatorTask {
  ValidatorTask(this.validator, this.result);
  final DoctorValidator validator;
  final Future<ValidationResult> result;
}

/// The doctor consumes a set of workflows and validators and produces a human-readable
/// message from the diagnosis results.
class Doctor {
  Doctor({
    @required Logger logger,
    @required DoctorValidatorsProvider doctorValidatorsProvider,
    @required OutputPreferences outputPreferences,
  }) : _logger = logger,
       _doctorValidatorsProvider = doctorValidatorsProvider,
       _outputPreferences = outputPreferences;

  final Logger _logger;
  final DoctorValidatorsProvider _doctorValidatorsProvider;
  final OutputPreferences _outputPreferences;

  List<DoctorValidator> get validators {
    return _doctorValidatorsProvider.validators;
  }

  List<Workflow> get workflows {
    return _doctorValidatorsProvider.workflows;
  }

  /// Return a list of [ValidatorTask] objects and starts validation on all
  /// objects in [validators].
  List<ValidatorTask> startValidatorTasks() => <ValidatorTask>[
    for (final DoctorValidator validator in validators)
      ValidatorTask(
        validator,
        // We use an asyncGuard() here to be absolutely certain that
        // DoctorValidators do not result in an uncaught exception. Since the
        // Future returned by the asyncGuard() is not awaited, we pass an
        // onError callback to it and translate errors into ValidationResults.
        asyncGuard<ValidationResult>(
          validator.validate,
          onError: (Object exception, StackTrace stackTrace) {
            return ValidationResult.crash(exception, stackTrace);
          },
        ),
      ),
  ];

  /// Print a summary of the state of the tooling, as well as how to get more info.
  Future<void> summary() async {
    _logger.printStatus(await _summaryText());
  }

  Future<String> _summaryText() async {
    final StringBuffer buffer = StringBuffer();

    bool missingComponent = false;
    bool sawACrash = false;

    for (final DoctorValidator validator in validators) {
      final StringBuffer lineBuffer = StringBuffer();
      ValidationResult result;
      try {
        result = await asyncGuard<ValidationResult>(() => validator.validate());
      } on Exception catch (exception) {
        // We're generating a summary, so drop the stack trace.
        result = ValidationResult.crash(exception);
      }
      lineBuffer.write('${result.coloredLeadingBox} ${validator.title}: ');
      switch (result.type) {
        case ValidationType.crash:
          lineBuffer.write('the doctor check crashed without a result.');
          sawACrash = true;
          break;
        case ValidationType.missing:
          lineBuffer.write('is not installed.');
          break;
        case ValidationType.partial:
          lineBuffer.write('is partially installed; more components are available.');
          break;
        case ValidationType.notAvailable:
          lineBuffer.write('is not available.');
          break;
        case ValidationType.installed:
          lineBuffer.write('is fully installed.');
          break;
      }

      if (result.statusInfo != null) {
        lineBuffer.write(' (${result.statusInfo})');
      }

      buffer.write(wrapText(
        lineBuffer.toString(),
        hangingIndent: result.leadingBox.length + 1,
        columnWidth: _outputPreferences.wrapColumn,
        shouldWrap: _outputPreferences.wrapText,
      ));
      buffer.writeln();

      if (result.type != ValidationType.installed) {
        missingComponent = true;
      }
    }

    if (sawACrash) {
      buffer.writeln();
      buffer.writeln('Run "flutter doctor" for information about why a doctor check crashed.');
    }

    if (missingComponent) {
      buffer.writeln();
      buffer.writeln('Run "flutter doctor" for information about installing additional components.');
    }

    return buffer.toString();
  }

  Future<bool> checkRemoteArtifacts(String engineRevision) async {
    return globals.cache.areRemoteArtifactsAvailable(engineVersion: engineRevision);
  }

  /// Print information about the state of installed tooling.
  Future<bool> diagnose({ bool androidLicenses = false, bool verbose = true, bool showColor = true }) async {
    if (androidLicenses) {
      return AndroidLicenseValidator.runLicenseManager();
    }

    if (!verbose) {
      _logger.printStatus('Doctor summary (to see all details, run flutter doctor -v):');
    }
    bool doctorResult = true;
    int issues = 0;
    final Terminal terminal = _logger.terminal;
    for (final ValidatorTask validatorTask in startValidatorTasks()) {
      final DoctorValidator validator = validatorTask.validator;
      final Status status = Status.withSpinner(
        timeout: timeoutConfiguration.fastOperation,
        slowWarningCallback: () => validator.slowWarning,
        timeoutConfiguration: timeoutConfiguration,
        stopwatch: Stopwatch(),
        terminal: terminal,
      );
      ValidationResult result;
      try {
        result = await validatorTask.result;
        status.stop();
      } on Exception catch (exception, stackTrace) {
        result = ValidationResult.crash(exception, stackTrace);
        status.cancel();
      }

      switch (result.type) {
        case ValidationType.crash:
          doctorResult = false;
          issues += 1;
          break;
        case ValidationType.missing:
          doctorResult = false;
          issues += 1;
          break;
        case ValidationType.partial:
        case ValidationType.notAvailable:
          issues += 1;
          break;
        case ValidationType.installed:
          break;
      }

      DoctorResultEvent(validator: validator, result: result).send();

      final String leadingBox = showColor ? result.coloredLeadingBox(terminal) : result.leadingBox;
      if (result.statusInfo != null) {
        _logger.printStatus('$leadingBox ${validator.title} (${result.statusInfo})',
            hangingIndent: result.leadingBox.length + 1);
      } else {
        _logger.printStatus('$leadingBox ${validator.title}',
            hangingIndent: result.leadingBox.length + 1);
      }

      for (final ValidationMessage message in result.messages) {
        if (message.type != ValidationMessageType.information || verbose == true) {
          int hangingIndent = 2;
          int indent = 4;
          final String indicator = showColor ? message.coloredIndicator(terminal) : message.indicator;
          for (final String line in '$indicator ${message.message}'.split('\n')) {
            _logger.printStatus(line, hangingIndent: hangingIndent, indent: indent, emphasis: true);
            // Only do hanging indent for the first line.
            hangingIndent = 0;
            indent = 6;
          }
          if (message.contextUrl != null) {
            _logger.printStatus('🔨 ${message.contextUrl}', hangingIndent: hangingIndent, indent: indent, emphasis: true);
          }
        }
      }
      if (verbose) {
        _logger.printStatus('');
      }
    }

    // Make sure there's always one line before the summary even when not verbose.
    if (!verbose) {
      _logger.printStatus('');
    }

    if (issues > 0) {
      _logger.printStatus('${showColor ? terminal.color('!', TerminalColor.yellow) : '!'}'
        ' Doctor found issues in $issues categor${issues > 1 ? "ies" : "y"}.', hangingIndent: 2);
    } else {
      _logger.printStatus('${showColor ? terminal.color('•', TerminalColor.green) : '•'}'
        ' No issues found!', hangingIndent: 2);
    }

    return doctorResult;
  }

  bool get canListAnything => workflows.any((Workflow workflow) => workflow.canListDevices);

  bool get canLaunchAnything {
    if (FlutterTesterDevices.showFlutterTesterDevice) {
      return true;
    }
    return workflows.any((Workflow workflow) => workflow.canLaunchDevices);
  }
}

/// A series of tools and required install steps for a target platform (iOS or Android).
///
/// A workflow determines whether device discovery and doctor validators
/// are supported by a platform. For example, a workflow may use the [Platform]
/// to determine if it is applicable. This is done by the iOS workflow to
/// disable attempts to discover devices or provide doctor validation on non-macOS
/// platforms. Another example is the use of [FeatureFlags] to conditionally
/// enable web and desktop workflows if the user has opted in.
abstract class Workflow {
  const Workflow();

  /// Whether the workflow applies to this platform (as in, should we ever try and use it).
  bool get appliesToHostPlatform;

  /// Are we functional enough to list devices?
  bool get canListDevices;

  /// Could this thing launch *something*? It may still have minor issues.
  bool get canLaunchDevices;

  /// Whether the workflow is capable of listing emulators or simulators.
  ///
  /// This will always return false for workflows that do not support
  /// emulators/simulators, such as web or desktop.
  bool get canListEmulators;
}

enum ValidationType {
  crash,
  missing,
  partial,
  notAvailable,
  installed,
}

enum ValidationMessageType {
  error,
  hint,
  information,
}

abstract class DoctorValidator {
  const DoctorValidator(this.title);

  /// This is displayed in the CLI.
  final String title;

  String get slowWarning => 'This is taking an unexpectedly long time...';

  Future<ValidationResult> validate();
}

/// A validator that runs other [DoctorValidator]s and combines their output
/// into a single [ValidationResult]. It uses the title of the first validator
/// passed to the constructor and reports the statusInfo of the first validator
/// that provides one. Other titles and statusInfo strings are discarded.
class GroupedValidator extends DoctorValidator {
  GroupedValidator(this.subValidators) : super(subValidators[0].title);

  final List<DoctorValidator> subValidators;

  List<ValidationResult> _subResults;

  /// Sub-validator results.
  ///
  /// To avoid losing information when results are merged, the sub-results are
  /// cached on this field when they are available. The results are in the same
  /// order as the sub-validator list.
  List<ValidationResult> get subResults => _subResults;

  @override
  String get slowWarning => _currentSlowWarning;
  String _currentSlowWarning = 'Initializing...';

  @override
  Future<ValidationResult> validate() async {
    final List<ValidatorTask> tasks = <ValidatorTask>[
      for (final DoctorValidator validator in subValidators)
        ValidatorTask(
          validator,
          asyncGuard<ValidationResult>(() => validator.validate()),
        ),
    ];

    final List<ValidationResult> results = <ValidationResult>[];
    for (final ValidatorTask subValidator in tasks) {
      _currentSlowWarning = subValidator.validator.slowWarning;
      try {
        results.add(await subValidator.result);
      } on Exception catch (exception, stackTrace) {
        results.add(ValidationResult.crash(exception, stackTrace));
      }
    }
    _currentSlowWarning = 'Merging results...';
    return _mergeValidationResults(results);
  }

  ValidationResult _mergeValidationResults(List<ValidationResult> results) {
    assert(results.isNotEmpty, 'Validation results should not be empty');
    _subResults = results;
    ValidationType mergedType = results[0].type;
    final List<ValidationMessage> mergedMessages = <ValidationMessage>[];
    String statusInfo;

    for (final ValidationResult result in results) {
      statusInfo ??= result.statusInfo;
      switch (result.type) {
        case ValidationType.installed:
          if (mergedType == ValidationType.missing) {
            mergedType = ValidationType.partial;
          }
          break;
        case ValidationType.notAvailable:
        case ValidationType.partial:
          mergedType = ValidationType.partial;
          break;
        case ValidationType.crash:
        case ValidationType.missing:
          if (mergedType == ValidationType.installed) {
            mergedType = ValidationType.partial;
          }
          break;
        default:
          throw 'Unrecognized validation type: ' + result.type.toString();
      }
      mergedMessages.addAll(result.messages);
    }

    return ValidationResult(mergedType, mergedMessages,
        statusInfo: statusInfo);
  }
}

@immutable
class ValidationResult {
  /// [ValidationResult.type] should only equal [ValidationResult.installed]
  /// if no [messages] are hints or errors.
  const ValidationResult(this.type, this.messages, { this.statusInfo });

  factory ValidationResult.crash(Object error, [StackTrace stackTrace]) {
    return ValidationResult(ValidationType.crash, <ValidationMessage>[
      const ValidationMessage.error(
          'Due to an error, the doctor check did not complete. '
          'If the error message below is not helpful, '
          'please let us know about this issue at https://github.com/flutter/flutter/issues.'),
      ValidationMessage.error('$error'),
      if (stackTrace != null)
          // Stacktrace is informational. Printed in verbose mode only.
          ValidationMessage('$stackTrace'),
    ], statusInfo: 'the doctor check crashed');
  }

  final ValidationType type;
  // A short message about the status.
  final String statusInfo;
  final List<ValidationMessage> messages;

  String get leadingBox {
    assert(type != null);
    switch (type) {
      case ValidationType.crash:
        return '[☠]';
      case ValidationType.missing:
        return '[✗]';
      case ValidationType.installed:
        return '[✓]';
      case ValidationType.notAvailable:
      case ValidationType.partial:
        return '[!]';
    }
    return null;
  }

  String coloredLeadingBox(Terminal terminal) {
    assert(type != null);
    switch (type) {
      case ValidationType.crash:
        return terminal.color(leadingBox, TerminalColor.red);
      case ValidationType.missing:
        return terminal.color(leadingBox, TerminalColor.red);
      case ValidationType.installed:
        return terminal.color(leadingBox, TerminalColor.green);
      case ValidationType.notAvailable:
      case ValidationType.partial:
        return terminal.color(leadingBox, TerminalColor.yellow);
    }
    return null;
  }

  /// The string representation of the type.
  String get typeStr {
    assert(type != null);
    switch (type) {
      case ValidationType.crash:
        return 'crash';
      case ValidationType.missing:
        return 'missing';
      case ValidationType.installed:
        return 'installed';
      case ValidationType.notAvailable:
        return 'notAvailable';
      case ValidationType.partial:
        return 'partial';
    }
    return null;
  }
}

/// A status line for the flutter doctor validation to display.
///
/// The [message] is required and represents either an informational statement
/// about the particular doctor validation that passed, or more context
/// on the cause and/or solution to the validation failure.
@immutable
class ValidationMessage {
  /// Create a validation message with information for a passing validatior.
  ///
  /// By default this is not displayed unless the doctor is run in
  /// verbose mode.
  ///
  /// The [contextUrl] may be supplied to link to external resources. This
  /// is displayed after the informative message in verbose modes.
  const ValidationMessage(this.message, {this.contextUrl}) : type = ValidationMessageType.information;

  /// Create a validation message with information for a failing validator.
  const ValidationMessage.error(this.message)
    : type = ValidationMessageType.error,
      contextUrl = null;

  /// Create a validation message with information for a partially failing
  /// validator.
  const ValidationMessage.hint(this.message)
    : type = ValidationMessageType.hint,
      contextUrl = null;

  final ValidationMessageType type;
  final String contextUrl;
  final String message;

  bool get isError => type == ValidationMessageType.error;

  bool get isHint => type == ValidationMessageType.hint;

  String get indicator {
    switch (type) {
      case ValidationMessageType.error:
        return '✗';
      case ValidationMessageType.hint:
        return '!';
      case ValidationMessageType.information:
        return '•';
    }
    return null;
  }

  String coloredIndicator(Terminal terminal) {
    switch (type) {
      case ValidationMessageType.error:
        return terminal.color(indicator, TerminalColor.red);
      case ValidationMessageType.hint:
        return terminal.color(indicator, TerminalColor.yellow);
      case ValidationMessageType.information:
        return terminal.color(indicator, TerminalColor.green);
    }
    return null;
  }

  @override
  String toString() => '{$type, $message, $contextUrl}';

  @override
  bool operator ==(Object other) {
    return other is ValidationMessage
        && other.message == message
        && other.type == type
        && other.contextUrl == contextUrl;
  }

  @override
  int get hashCode => type.hashCode ^ message.hashCode ^ contextUrl.hashCode;
}

/// A validator that checks the version of flutter, as well as some auxillary information
/// such as the pub or flutter cache overrides.
///
/// This is primarily useful for diagnosing issues on Github bug reports by displaying
/// specific commit information.
class FlutterValidator extends DoctorValidator {
  FlutterValidator({
    @required Platform platform,
    @required FlutterVersion Function() flutterVersion,
    @required UserMessages userMessages,
    @required FileSystem fileSystem,
    @required Artifacts artifacts,
    @required ProcessManager processManager,
  }) : _flutterVersion = flutterVersion,
       _platform = platform,
       _userMessages = userMessages,
       _fileSystem = fileSystem,
       _artifacts = artifacts,
       _processManager = processManager,
       super('Flutter');

  final Platform _platform;
  final FlutterVersion Function() _flutterVersion;
  final UserMessages _userMessages;
  final FileSystem _fileSystem;
  final Artifacts _artifacts;
  final ProcessManager _processManager;

  @override
  Future<ValidationResult> validate() async {
    final List<ValidationMessage> messages = <ValidationMessage>[];
    ValidationType valid = ValidationType.installed;
    String versionChannel;
    String frameworkVersion;

    try {
      final FlutterVersion version = _flutterVersion();
      versionChannel = version.channel;
      frameworkVersion = version.frameworkVersion;
      messages.add(ValidationMessage(_userMessages.flutterVersion(
        frameworkVersion,
        Cache.flutterRoot,
      )));
      messages.add(ValidationMessage(_userMessages.flutterRevision(
        version.frameworkRevisionShort,
        version.frameworkAge,
        version.frameworkDate,
      )));
      messages.add(ValidationMessage(_userMessages.engineRevision(version.engineRevisionShort)));
      messages.add(ValidationMessage(_userMessages.dartRevision(version.dartSdkVersion)));
      if (_platform.environment.containsKey('PUB_HOSTED_URL')) {
        messages.add(ValidationMessage(_userMessages.pubMirrorURL(_platform.environment['PUB_HOSTED_URL'])));
      }
      if (_platform.environment.containsKey('FLUTTER_STORAGE_BASE_URL')) {
        messages.add(ValidationMessage(_userMessages.flutterMirrorURL(_platform.environment['FLUTTER_STORAGE_BASE_URL'])));
      }
    } on VersionCheckError catch (e) {
      messages.add(ValidationMessage.error(e.message));
      valid = ValidationType.partial;
    }

    // Check that the binaries we downloaded for this platform actually run on it.
    // this requires the doctor to uncondtionally download the android artifacts.
    final String genSnapshotPath = _artifacts.getArtifactPath(Artifact.genSnapshot);
    if (_fileSystem.file(genSnapshotPath).existsSync() && !_genSnapshotRuns(genSnapshotPath)) {
      final StringBuffer buffer = StringBuffer();
      buffer.writeln(_userMessages.flutterBinariesDoNotRun);
      if (_platform.isLinux) {
        buffer.writeln(_userMessages.flutterBinariesLinuxRepairCommands);
      }
      messages.add(ValidationMessage.error(buffer.toString()));
      valid = ValidationType.partial;
    }

    return ValidationResult(
      valid,
      messages,
      statusInfo: _userMessages.flutterStatusInfo(
        versionChannel,
        frameworkVersion,
        _platform.name,
        _platform.localeName,
      ),
    );
  }

  bool _genSnapshotRuns(String genSnapshotPath) {
    const int kExpectedExitCode = 255;
    try {
      return _processManager.runSync(<String>[genSnapshotPath]).exitCode == kExpectedExitCode;
    } on Exception {
      return false;
    }
  }
}

/// A validator that is only displayed if no supported first party Flutter IDE
/// is detected.
class NoIdeValidator extends DoctorValidator {
  NoIdeValidator({
    @required UserMessages userMessages,
  }) : _userMessages = userMessages,
       super('Flutter IDE Support');

  final UserMessages _userMessages;

  @override
  Future<ValidationResult> validate() async {
    return ValidationResult(
      ValidationType.missing,
      <ValidationMessage>[
        ValidationMessage(_userMessages.noIdeInstallationInfo),
      ],
      statusInfo: _userMessages.noIdeStatusInfo,
    );
  }
}

/// A validator that detects all attached devices and displays any diagnostic
/// messages returned by the device discovery.
///
/// Certain device types may end up in a "locked" state, due to an unaccepted
/// permission dialog, required update, or other unknown reason. This validator
/// is intended to surface the information here that would otherwise be too
/// verbose to display during flutter run.
class DeviceValidator extends DoctorValidator {
  DeviceValidator({
    @required DeviceManager deviceManager,
    @required UserMessages userMessages,
  }) : _deviceManager = deviceManager,
       _userMessages = userMessages,
       super('Connected device');

  final DeviceManager _deviceManager;
  final UserMessages _userMessages;

  @override
  String get slowWarning => 'Scanning for devices is taking a long time...';

  @override
  Future<ValidationResult> validate() async {
    final List<Device> devices = await _deviceManager.getAllConnectedDevices();
    List<ValidationMessage> installedMessages = <ValidationMessage>[];
    if (devices.isNotEmpty) {
      installedMessages = await Device.descriptions(devices)
          .map<ValidationMessage>((String msg) => ValidationMessage(msg)).toList();
    }

    List<ValidationMessage> diagnosticMessages = <ValidationMessage>[];
    final List<String> diagnostics = await _deviceManager.getDeviceDiagnostics();
    if (diagnostics.isNotEmpty) {
      diagnosticMessages = diagnostics.map<ValidationMessage>((String message) => ValidationMessage.hint(message)).toList();
    } else if (devices.isEmpty) {
      diagnosticMessages = <ValidationMessage>[ValidationMessage.hint(_userMessages.devicesMissing)];
    }

    if (devices.isEmpty) {
      return ValidationResult(ValidationType.notAvailable, diagnosticMessages);
    }
    if (diagnostics.isNotEmpty) {
      installedMessages.addAll(diagnosticMessages);
      return ValidationResult(
        ValidationType.installed,
        installedMessages,
        statusInfo: _userMessages.devicesAvailable(devices.length)
      );
    }
    return ValidationResult(
      ValidationType.installed,
      installedMessages,
      statusInfo: _userMessages.devicesAvailable(devices.length)
    );
  }
}

class ValidatorWithResult extends DoctorValidator {
  ValidatorWithResult(String title, this.result) : super(title);

  final ValidationResult result;

  @override
  Future<ValidationResult> validate() async => result;
}
