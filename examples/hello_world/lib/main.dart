// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:ui' as ui;
import 'package:flutter/widgets.dart';

Future<void> main() async {
  await ui.webOnlyInitializePlatform(); // ignore: undefined_function
  runApp(const Center(child: Text('hello, goodbye', textDirection: TextDirection.ltr,)));
}
