// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:flutter/material.dart';
import 'package:flutter/widgets.dart';
import 'package:hello_world/printer.dart';
import 'package:hello_world/printer_app.dart';

void main() => runApp(ExampleWidget());

class ExampleWidget extends StatefulWidget {
  @override
  _ExampleState createState() => _ExampleState();
}

class _ExampleState extends State<ExampleWidget> {
  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      home: Scaffold(
        body: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: <Widget>[
          RaisedButton(
            child: const Text('Press Me Thrice, Four Times'),
            onPressed: () async {
              (await Example.create(MachineModule())).getMachine2().otherMessage();
            },
          )
        ],)
      ),
    );
  }
}
