// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file

import '../base/file_system.dart';
import '../base/platform.dart';
import '../project.dart';

const List<String> _kSupportedYears = <String>['2017', '2018'];
const List<String> _kSupportedFlavors = <String>['Community', 'Professional', 'Enterprise', 'Preview'];

/// Writes Generated.props, a configuration format for msbuild.
void updateGeneratedProps(
  WindowsProject windowsProject, {
  String flutterRoot,
  bool trackWidgetCreation,
}) {
  final StringBuffer output = StringBuffer('''<?xml version="1.0" encoding="utf-8"?>
<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ImportGroup Label="PropertySheets" />
  <PropertyGroup Label="UserMacros">
    <FLUTTER_ROOT>$flutterRoot</FLUTTER_ROOT>
''');
  if (trackWidgetCreation) {
    output.writeln('    <EXTRA_BUNDLE_FLAGS>--track-widget-creation</EXTRA_BUNDLE_FLAGS>');
  }
  output.write('''  </PropertyGroup>
  <PropertyGroup />
  <ItemDefinitionGroup />
  <ItemGroup>
    <BuildMacro Include="FLUTTER_ROOT">
      <Value>\$($flutterRoot)</Value>
      <EnvironmentVariable>true</EnvironmentVariable>
    </BuildMacro>
''');
  if (trackWidgetCreation) {
    output.write('''    <BuildMacro Include="EXTRA_BUNDLE_FLAGS">
      <Value>\$(--track-widget-creation)</Value>
      <EnvironmentVariable>true</EnvironmentVariable>
    </BuildMacro>
''');
  }
  output.write('''  </ItemGroup>
</Project>''');
  windowsProject.generatedPropsFile
    ..createSync()
    ..writeAsStringSync(output.toString());
}

/// Attempt to locate vcvars64.bat in a visual studio installation.
///
/// If the file cannot be found, returns `null`.
String findVcVars() {
  final String programPath = platform.environment['PROGRAMFILES(X86)'];
  final String pathPrefix = fs.path.join(programPath, 'Microsoft Visual Studio');
  final String pathSuffx = fs.path.join('VC', 'Auxiliary', 'Build', 'vcvars64.bat');
  for (String year in _kSupportedYears) {
    for (String flavor in _kSupportedFlavors) {
      final String testPath = fs.path.join(pathPrefix, year, flavor, pathSuffx);
      final File testFile = fs.file(testPath);
      if (testFile.existsSync()) {
        return testFile.path;
      }
    }
  }
  // Could not find vcvars.
  return null;
}