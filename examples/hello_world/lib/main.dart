// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:flutter/material.dart';
import 'package:flutter/widgets.dart';

void main() {
  runApp(MaterialApp(home: Example()));
}

class Example extends StatefulWidget {
  @override
  _ExampleState2 createState() => _ExampleState2();
}

class _ExampleState2 extends State<Example> {
  @override
  Widget build(BuildContext context) {
    WidgetsBinding.instance.hackElement = context;
    return Scaffold(
      body: Center(
        child: Transform.rotate(
          angle: 0,
          child: Container(
            width: 100,
            height: 100,
            color: Colors.green,
          )
        ),
      ),
    );
  }
}
