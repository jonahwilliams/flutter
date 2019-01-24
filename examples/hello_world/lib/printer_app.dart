import 'package:hello_world/printer.dart';
import 'package:inject/inject.dart';
import 'package:hello_world/printer_app.inject.dart' as generated;

@Injector([MachineModule])
abstract class Example {
  static const create = generated.Example$Injector.create;

  @provide
  Machine getMachine2();
}