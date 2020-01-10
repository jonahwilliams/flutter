// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:package_config/packages_file.dart' as packages_file;

import '../base/file_system.dart';
import '../globals.dart' as globals;

const String kPackagesFileName = '.packages';


class PackageMap {
  // TODO(jonahwilliams): remove when fully migrating to context free.
  PackageMap(this.packagesPath, [this._fileSystem, this._isWindows]);

  static String get globalPackagesPath => _globalPackagesPath ?? kPackagesFileName;

  static String get globalGeneratedPackagesPath => globals.fs.path.setExtension(globalPackagesPath, '.generated');

  static set globalPackagesPath(String value) {
    _globalPackagesPath = value;
  }

  static bool get isUsingCustomPackagesPath => _globalPackagesPath != null;

  static String _globalPackagesPath;

  final String packagesPath;
  final FileSystem _fileSystem;
  final bool _isWindows;

  /// Load and parses the .packages file.
  void load() {
    _map ??= _parse(packagesPath);
  }

  Map<String, Uri> _parse(String packagesPath) {
    final List<int> source = (_fileSystem ?? globals.fs).file(packagesPath).readAsBytesSync();
    return packages_file.parse(source, Uri.file(packagesPath, windows: _isWindows ?? globals.platform.isWindows));
  }

  Map<String, Uri> get map {
    load();
    return _map;
  }
  Map<String, Uri> _map;

  /// Returns the path to [packageUri].
  String pathForPackage(Uri packageUri) => uriForPackage(packageUri).path;

  /// Returns the path to [packageUri] as URL.
  Uri uriForPackage(Uri packageUri) {
    assert(packageUri.scheme == 'package');
    final List<String> pathSegments = packageUri.pathSegments.toList();
    final String packageName = pathSegments.removeAt(0);
    final Uri packageBase = map[packageName];
    if (packageBase == null) {
      return null;
    }
    final String packageRelativePath = (_fileSystem ?? globals.fs).path.joinAll(pathSegments);
    return packageBase.resolveUri((_fileSystem ?? globals.fs).path.toUri(packageRelativePath));
  }

  String checkValid() {
    if ((_fileSystem ?? globals.fs).isFileSync(packagesPath)) {
      return null;
    }
    String message = '$packagesPath does not exist.';
    final String pubspecPath = (_fileSystem ?? globals.fs).path
      .absolute((_fileSystem ?? globals.fs).path.dirname(packagesPath), 'pubspec.yaml');
    if ((_fileSystem ?? globals.fs).isFileSync(pubspecPath)) {
      message += '\nDid you run "flutter pub get" in this directory?';
    } else {
      message += '\nDid you run this command from the same directory as your pubspec.yaml file?';
    }
    return message;
  }
}
