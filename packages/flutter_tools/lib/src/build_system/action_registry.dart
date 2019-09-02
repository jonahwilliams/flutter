// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';

import 'build_system.dart';
import 'targets/build_actions.dart';

/// Registers rule implementations that are looked up by the [RuleParser].
class BuildActionRegistry {
  BuildActionRegistry({
    Map<String, BuildAction> actions = const <String, BuildAction>{},
  }) : _actions = actions;

  final Map<String, BuildAction> _actions;

  /// Register a [BuildAction] under [name].
  ///
  /// Throws a [StateError] If another action is already registered to that
  /// name.
  void register(String name, BuildAction buildAction) {
    if (_actions.containsKey(name)) {
      throw StateError('$name is already registered.');
    }
    _actions[name] = buildAction;
  }

  /// Resolve [name] to a [BuildAction] instance.
  ///
  /// Throws a [StateError] If no action is registered with that name.
  BuildAction resolve(String name) {
    if (!_actions.containsKey(name)) {
      throw StateError('$name is not registered.');
    }
    return _actions[name];
  }
}
