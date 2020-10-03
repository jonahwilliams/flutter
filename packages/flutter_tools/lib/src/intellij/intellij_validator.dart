// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:meta/meta.dart';

import '../base/file_system.dart';
import '../base/platform.dart';
import '../base/user_messages.dart';
import '../base/version.dart';
import '../doctor.dart';
import '../globals.dart' as globals;
import '../ios/plist_parser.dart';
import 'intellij.dart';

/// A doctor validator for both Intellij and Android Studio.
abstract class IntelliJValidator extends DoctorValidator {
  IntelliJValidator(String title, this.installPath, {
    @required FileSystem fileSystem,
  }) : _fileSystem = fileSystem,
       super(title);

  final String installPath;
  final FileSystem _fileSystem;

  String get version;
  String get pluginsPath;

  static final Map<String, String> _idToTitle = <String, String>{
    'IntelliJIdea': 'IntelliJ IDEA Ultimate Edition',
    'IdeaIC': 'IntelliJ IDEA Community Edition',
  };

  static final Version kMinIdeaVersion = Version(2017, 1, 0);

  static Iterable<DoctorValidator> installedValidators(FileSystem fileSystem, Platform platform) {
    if (platform.isLinux || platform.isWindows) {
      return IntelliJValidatorOnLinuxAndWindows.installed(fileSystem);
    }
    if (platform.isMacOS) {
      return IntelliJValidatorOnMac.installed(fileSystem);
    }
    return <DoctorValidator>[];
  }

  @override
  Future<ValidationResult> validate() async {
    final List<ValidationMessage> messages = <ValidationMessage>[];

    if (pluginsPath == null) {
      messages.add(const ValidationMessage.error('Invalid IntelliJ version number.'));
    } else {
      messages.add(ValidationMessage(userMessages.intellijLocation(installPath)));

      final IntelliJPlugins plugins = IntelliJPlugins(pluginsPath, fileSystem: _fileSystem);
      plugins.validatePackage(
        messages,
        <String>['flutter-intellij', 'flutter-intellij.jar'],
        'Flutter',
        IntelliJPlugins.kIntellijFlutterPluginUrl,
        minVersion: IntelliJPlugins.kMinFlutterPluginVersion,
      );
      plugins.validatePackage(
        messages,
        <String>['Dart'],
        'Dart',
        IntelliJPlugins.kIntellijDartPluginUrl,
      );

      if (_hasIssues(messages)) {
        messages.add(ValidationMessage(userMessages.intellijPluginInfo));
      }

      _validateIntelliJVersion(messages, kMinIdeaVersion);
    }

    return ValidationResult(
      _hasIssues(messages) ? ValidationType.partial : ValidationType.installed,
      messages,
      statusInfo: userMessages.intellijStatusInfo(version),
    );
  }

  bool _hasIssues(List<ValidationMessage> messages) {
    return messages.any((ValidationMessage message) => message.isError);
  }

  void _validateIntelliJVersion(List<ValidationMessage> messages, Version minVersion) {
    // Ignore unknown versions.
    if (minVersion == Version.unknown) {
      return;
    }

    final Version installedVersion = Version.parse(version);
    if (installedVersion == null) {
      return;
    }

    if (installedVersion < minVersion) {
      messages.add(ValidationMessage.error(userMessages.intellijMinimumVersion(minVersion.toString())));
    }
  }
}

/// A linux and windows specific implementation of the intellij validator.
class IntelliJValidatorOnLinuxAndWindows extends IntelliJValidator {
  IntelliJValidatorOnLinuxAndWindows(String title, this.version, String installPath, this.pluginsPath, {
    @required FileSystem fileSystem,
  }) : super(title, installPath, fileSystem: fileSystem);

  @override
  final String version;

  @override
  final String pluginsPath;

  static Iterable<DoctorValidator> installed(FileSystem fileSystem) {
    final List<DoctorValidator> validators = <DoctorValidator>[];
    if (globals.fsUtils.homeDirPath == null) {
      return validators;
    }

    void addValidator(String title, String version, String installPath, String pluginsPath) {
      final IntelliJValidatorOnLinuxAndWindows validator =
        IntelliJValidatorOnLinuxAndWindows(title, version, installPath, pluginsPath, fileSystem: fileSystem);
      for (int index = 0; index < validators.length; ++index) {
        final DoctorValidator other = validators[index];
        if (other is IntelliJValidatorOnLinuxAndWindows && validator.installPath == other.installPath) {
          if (validator.version.compareTo(other.version) > 0) {
            validators[index] = validator;
          }
          return;
        }
      }
      validators.add(validator);
    }

    final Directory homeDir = globals.fs.directory(globals.fsUtils.homeDirPath);
    for (final Directory dir in homeDir.listSync().whereType<Directory>()) {
      final String name = globals.fs.path.basename(dir.path);
      IntelliJValidator._idToTitle.forEach((String id, String title) {
        if (name.startsWith('.$id')) {
          final String version = name.substring(id.length + 1);
          String installPath;
          try {
            installPath = globals.fs.file(globals.fs.path.join(dir.path, 'system', '.home')).readAsStringSync();
          } on Exception {
            // ignored
          }
          if (installPath != null && globals.fs.isDirectorySync(installPath)) {
            final String pluginsPath = globals.fs.path.join(dir.path, 'config', 'plugins');
            addValidator(title, version, installPath, pluginsPath);
          }
        }
      });
    }
    return validators;
  }
}

/// A macOS specific implementation of the intellij validator.
class IntelliJValidatorOnMac extends IntelliJValidator {
  IntelliJValidatorOnMac(String title, this.id, String installPath, {
    @required FileSystem fileSystem,
  }) : super(title, installPath, fileSystem: fileSystem);

  final String id;

  static final Map<String, String> _dirNameToId = <String, String>{
    'IntelliJ IDEA.app': 'IntelliJIdea',
    'IntelliJ IDEA Ultimate.app': 'IntelliJIdea',
    'IntelliJ IDEA CE.app': 'IdeaIC',
  };

  static Iterable<DoctorValidator> installed(FileSystem fileSystem) {
    final List<DoctorValidator> validators = <DoctorValidator>[];
    final List<String> installPaths = <String>[
      '/Applications',
      fileSystem.path.join(globals.fsUtils.homeDirPath, 'Applications'),
    ];

    void checkForIntelliJ(Directory dir) {
      final String name = fileSystem.path.basename(dir.path);
      _dirNameToId.forEach((String dirName, String id) {
        if (name == dirName) {
          final String title = IntelliJValidator._idToTitle[id];
          validators.add(IntelliJValidatorOnMac(title, id, dir.path, fileSystem: fileSystem));
        }
      });
    }

    try {
      final Iterable<Directory> installDirs = installPaths
        .map(fileSystem.directory)
        .map<List<FileSystemEntity>>((Directory dir) => dir.existsSync() ? dir.listSync() : <FileSystemEntity>[])
        .expand<FileSystemEntity>((List<FileSystemEntity> mappedDirs) => mappedDirs)
        .whereType<Directory>();
      for (final Directory dir in installDirs) {
        checkForIntelliJ(dir);
        if (!dir.path.endsWith('.app')) {
          for (final FileSystemEntity subdir in dir.listSync()) {
            if (subdir is Directory) {
              checkForIntelliJ(subdir);
            }
          }
        }
      }
    } on FileSystemException catch (e) {
      validators.add(ValidatorWithResult(
          userMessages.intellijMacUnknownResult,
          ValidationResult(ValidationType.missing, <ValidationMessage>[
            ValidationMessage.error(e.message),
          ]),
      ));
    }
    return validators;
  }

  @visibleForTesting
  String get plistFile {
    _plistFile ??= _fileSystem.path.join(installPath, 'Contents', 'Info.plist');
    return _plistFile;
  }
  String _plistFile;

  @override
  String get version {
    _version ??= globals.plistParser.getValueFromFile(
        plistFile,
        PlistParser.kCFBundleShortVersionStringKey,
      ) ?? 'unknown';
    return _version;
  }
  String _version;

  @override
  String get pluginsPath {
    if (_pluginsPath != null) {
      return _pluginsPath;
    }

    final String altLocation = globals.plistParser
      .getValueFromFile(plistFile, 'JetBrainsToolboxApp');

    if (altLocation != null) {
      _pluginsPath = altLocation + '.plugins';
      return _pluginsPath;
    }

    final List<String> split = version.split('.');
    if (split.length < 2) {
      return null;
    }
    final String major = split[0];
    final String minor = split[1];

    final String homeDirPath = globals.fsUtils.homeDirPath;
    String pluginsPath = globals.fs.path.join(
      homeDirPath,
      'Library',
      'Application Support',
      'JetBrains',
      '$id$major.$minor',
      'plugins',
    );
    // Fallback to legacy location from < 2020.
    if (!globals.fs.isDirectorySync(pluginsPath)) {
      pluginsPath = globals.fs.path.join(
        homeDirPath,
        'Library',
        'Application Support',
        '$id$major.$minor',
      );
    }
    _pluginsPath = pluginsPath;

    return _pluginsPath;
  }
  String _pluginsPath;
}
