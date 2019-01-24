import 'package:inject/inject.dart';

abstract class Printer {
  void printMessage();
}

abstract class Stringer {
  String getMessage();
}

class PrintPrinter implements Printer {
  @override
  void printMessage() {
    print('printing twice');
  }
}

class MyStringer implements Stringer {
  @override
  String getMessage() => 'test';
}

@module
class MachineModule {
  @provide
  Printer providePrinter() => PrintPrinter();

  @provide
  Stringer provideStringer() => MyStringer();
}

class Machine {
  @provide
  Machine(this._printer, this._stringer);

  final Printer _printer;
  final Stringer _stringer;

  void printMessage() => _printer.printMessage();

  void otherMessage() => print(_stringer.getMessage() + 'adsaad');
}