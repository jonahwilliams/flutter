// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:flutter_tools/src/build_info.dart';
import 'package:yaml/yaml.dart';

import '../artifacts.dart';
import '../base/file_system.dart';
import '../globals.dart';
import 'build_system.dart';
import 'targets/build_actions.dart';
import 'targets/dart.dart';

/// Parses a .yaml file containing a map of build rules into a target graph.
///
/// May recursively parse further yaml files until all ruleas are known.
class RuleParser {
  RuleParser(this.files);

  final List<File> files;

  /// Produces a target that can be invoked for the [environment].
  Node parse(Environment environment, String definition) {
    final Stopwatch stopwatch = Stopwatch()..start();
    final Map<String, _BuildDefinition> defintions = <String, _BuildDefinition>{};
    final Map<String, _TargetDefinition> targets = <String, _TargetDefinition>{};
    for (File file in files) {
      final YamlMap root = loadYamlNode(file.readAsStringSync());
      final YamlMap rules = root['rules'];
      final YamlMap definitions = root['definitions'];

      if (rules != null) {
        for (String name in rules.keys) {
          final YamlMap values = rules[name];
          targets[name] = _TargetDefinition(
            name,
            _parseSources(values['inputs'], environment),
            _parseSources(values['outputs'], environment),
            values['type'] == 'copy' ? ActionType.copy : ActionType.dart,
            values['dependencies'] ?? const <String>[],
          );
        }
      }

      if (definitions != null) {
        for (String name in definitions.keys) {
          final YamlList values = definitions[name];
          final List<_TargetReference> targets = <_TargetReference>[];
          for (String target in values) {
            final Match match = _listPattern.matchAsPrefix(target);
            if (match == null) {
              targets.add(_StaticTargetReference(target));
            } else {
              final String head = match.group(1);
              final String body = match.group(2);
              for (String value in environment.dynamicValues[head]) {
                targets.add(_DynamicRuleTarget(value, body));
              }
            }
          }
          defintions[name] = _BuildDefinition(name, targets);
        }
      }
    }
    printTrace('prepared build in ${stopwatch.elapsedMilliseconds}');

    final _BuildDefinition buildDefinition = defintions[definition];
    final Node root = Node(
      definition,
      (Environment environment) => null,
      const <File>[],
      const <File>[],
      <Node>[],
    );
    for (_TargetReference targetReference in buildDefinition.targets) {
      if (targetReference is _StaticTargetReference) {
        root.dependencies.add(_inflateTarget(environment, targetReference.name, targets));
      } else {
        throw UnimplementedError();
      }
    }
    return root;
  }

  static Node _inflateTarget(Environment environment, String name, Map<String, _TargetDefinition> targets) {
    final _TargetDefinition target = targets[name];
    final List<Node> dependencies = target.dependencies
      .map((String target) => _inflateTarget(environment, target, targets))
      .toList();
    if (target.type == ActionType.copy) {
      return CopyNode(
        name,
        target.inputs,
        target.outputs,
        dependencies,
      );
    }
    return Node(
      name,
      buildActionRegistry.resolve(name),
      target.inputs,
      target.outputs,
      target.dependencies
        .map((String target) => _inflateTarget(environment, target, targets))
        .toList()
    );
  }

  static final Pattern _sourcePattern = RegExp('Source\((.*\))');
  static final Pattern _artifactPattern = RegExp('Artifact\((.*)\)');
  static final Pattern _listPattern = RegExp('ForEach\((.*),(.*)\)');
  static const Pattern _filePathPattern = '{file_path}';

  /// Supports Source(..), Artifact(..), or List(..)
  static List<File> _parseSources(YamlList sources, Environment environment) {
    final List<File> results = <File>[];
    final SourceVisitor sourceVisitor = SourceVisitor(environment);
    final TargetPlatform targetPlatform = getTargetPlatformForName(environment.defines[kTargetPlatform]);
    final BuildMode buildMode = getBuildModeForName(environment.dynamicValues[kBuildMode]);
    for (String value in sources) {
      final Match sourceMatch = _sourcePattern.matchAsPrefix(value);
      if (sourceMatch != null) {
        final String body = sourceMatch.group(1);
        sourceVisitor.visitPattern(body, false);
        continue;
      }
      final Match artifactMatch = _artifactPattern.matchAsPrefix(value);
      if (artifactMatch != null) {
        final String body = artifactMatch.group(1);
        sourceVisitor.visitArtifact(getArtifactForName(body), targetPlatform, buildMode);
        continue;
      }
      final Match listMatch = _listPattern.matchAsPrefix(value);
      if (listMatch != null) {
        final String head = listMatch.group(1);
        final String body = listMatch.group(2);
        for (String value in environment.dynamicValues[head]) {
          if (body == _filePathPattern) {
            results.add(fs.file(value));
          } else {
            sourceVisitor.visitPattern(body.replaceFirst(_filePathPattern, value), false);
          }
        }
        continue;
      }
      throw StateError('unresolved input pattern: $value');
    }
    results.addAll(sourceVisitor.sources);
    return results;
  }
}

enum ActionType {
  copy,
  dart,
}

class _TargetDefinition {
  const _TargetDefinition(this.name, this.inputs, this.outputs, this.type, this.dependencies);

  final String name;
  final List<File> inputs;
  final List<File> outputs;
  final List<String> dependencies;
  final ActionType type;
}

class _BuildDefinition {
  const _BuildDefinition(
    this.name,
    this.targets,
  );

  final String name;
  final List<_TargetReference> targets;
}

abstract class _TargetReference {
  const _TargetReference();
}

class _DynamicRuleTarget extends _TargetReference {
  const _DynamicRuleTarget(this.scopeName, this.templateRule);

  final String scopeName;
  final String templateRule;
}

class _StaticTargetReference extends _TargetReference {
  const _StaticTargetReference(this.name);

  final String name;
}

