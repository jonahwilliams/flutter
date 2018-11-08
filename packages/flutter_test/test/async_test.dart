// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_test/src/async.dart';

void main() {
  group('FakeAsync', () {
    final DateTime initialTime = DateTime(2000);
    const Duration elapseBy = Duration(days: 1);

    test('should set initial time', () {
      expect(FakeAsync().getClock(initialTime).now(), initialTime);
    });

    group('elapseBlocking', () {
      test('should elapse time without calling timers', () {
        bool timerCalled = false;
        final Timer timer = Timer(elapseBy ~/ 2, () => timerCalled = true);
        FakeAsync().elapseBlocking(elapseBy);
        expect(timerCalled, isFalse);
        timer.cancel();
      });

      test('should elapse time by the specified amount', () {
        final FakeAsync it = FakeAsync();
        it.elapseBlocking(elapseBy);
        expect(it.getClock(initialTime).now(), initialTime.add(elapseBy));
      });

      test('should throw when called with a negative duration', () {
        expect(() {
          FakeAsync().elapseBlocking(const Duration(days: -1));
        }, throwsA(isInstanceOf<AssertionError>()));
      });
    });

    group('elapse', () {
      test('should elapse time by the specified amount', () {
        FakeAsync().run((FakeAsync fakeAsync) {
          fakeAsync.elapse(elapseBy);
          expect(fakeAsync.getClock(initialTime).now(), initialTime.add(elapseBy));
        });
      });

      test('should throw ArgumentError when called with a negative duration',
          () {
        expect(() => FakeAsync().elapse(const Duration(days: -1)),
            throwsA(isInstanceOf<AssertionError>()));
      });

      test('should throw when called before previous call is complete', () {
        FakeAsync().run((FakeAsync fakeAsync) {
          dynamic error;
          Timer(elapseBy ~/ 2, () {
            try {
              fakeAsync.elapse(elapseBy);
            } catch (e) {
              error = e;
            }
          });
          fakeAsync.elapse(elapseBy);
          expect(error, isInstanceOf<AssertionError>());
        });
      });

      group('when creating timers', () {
        test('should call timers expiring before or at end time', () {
          FakeAsync().run((FakeAsync fakeAsync) {
            int beforeCallCount = 0;
            int atCallCount = 0;
            Timer(elapseBy ~/ 2, () {
              beforeCallCount++;
            });
            Timer(elapseBy, () {
              atCallCount++;
            });
            fakeAsync.elapse(elapseBy);
            expect(beforeCallCount, 1);
            expect(atCallCount, 1);
          });
        });

        test('should call timers expiring due to elapseBlocking', () {
          FakeAsync().run((FakeAsync fakeAsync) {
            bool secondaryCalled = false;
            Timer(elapseBy, () {
              fakeAsync.elapseBlocking(elapseBy);
            });
            Timer(elapseBy * 2, () {
              secondaryCalled = true;
            });
            fakeAsync.elapse(elapseBy);
            expect(secondaryCalled, isTrue);
            expect(fakeAsync.getClock(initialTime).now(), initialTime.add(elapseBy * 2));
          });
        });

        test('should call timers at their scheduled time', () {
          FakeAsync().run((FakeAsync fakeAsync) {
            DateTime calledAt;
            final List<DateTime> periodicCalledAt = <DateTime>[];
            Timer(elapseBy ~/ 2, () {
              calledAt = fakeAsync.getClock(initialTime).now();
            });
            Timer.periodic(elapseBy ~/ 2, (_) {
              periodicCalledAt.add(fakeAsync.getClock(initialTime).now());
            });
            fakeAsync.elapse(elapseBy);
            expect(calledAt, initialTime.add(elapseBy ~/ 2));
            expect(periodicCalledAt, <Duration>[elapseBy ~/ 2, elapseBy].map(initialTime.add));
          });
        });

        test('should not call timers expiring after end time', () {
          FakeAsync().run((FakeAsync fakeAsync) {
            int timerCallCount = 0;
            Timer(elapseBy * 2, () {
              timerCallCount++;
            });
            fakeAsync.elapse(elapseBy);
            expect(timerCallCount, 0);
          });
        });

        test('should not call canceled timers', () {
          FakeAsync().run((FakeAsync fakeAsync) {
            int timerCallCount = 0;
            final Timer timer = Timer(elapseBy ~/ 2, () {
              timerCallCount++;
            });
            timer.cancel();
            fakeAsync.elapse(elapseBy);
            expect(timerCallCount, 0);
          });
        });

        test('should call periodic timers each time the duration elapses', () {
          FakeAsync().run((FakeAsync fakeAsync) {
            int periodicCallCount = 0;
            Timer.periodic(elapseBy ~/ 10, (_) {
              periodicCallCount++;
            });
            fakeAsync.elapse(elapseBy);
            expect(periodicCallCount, 10);
          });
        });

        test('should call timers occurring at the same time in FIFO order', () {
          FakeAsync().run((FakeAsync fakeAsync) {
            final List<String> log = <String>[];
            Timer(elapseBy ~/ 2, () {
              log.add('1');
            });
            Timer(elapseBy ~/ 2, () {
              log.add('2');
            });
            fakeAsync.elapse(elapseBy);
            expect(log, <String>['1', '2']);
          });
        });

        test('should maintain FIFO order even with periodic timers', () {
          FakeAsync().run((FakeAsync fakeAsync) {
            final List<String> log = <String>[];
            Timer.periodic(elapseBy ~/ 2, (_) {
              log.add('periodic 1');
            });
            Timer(elapseBy ~/ 2, () {
              log.add('delayed 1');
            });
            Timer(elapseBy, () {
              log.add('delayed 2');
            });
            Timer.periodic(elapseBy, (_) {
              log.add('periodic 2');
            });
            fakeAsync.elapse(elapseBy);
            expect(log, <String>[
              'periodic 1',
              'delayed 1',
              'periodic 1',
              'delayed 2',
              'periodic 2'
            ]);
          });
        });

        test('should process microtasks surrounding each timer', () {
          FakeAsync().run((FakeAsync fakeAsync) {
            int microtaskCalls = 0;
            int timerCalls = 0;
            void scheduleMicrotasks() {
              for (int i = 0; i < 5; i++) {
                scheduleMicrotask(() => microtaskCalls++);
              }
            }
            scheduleMicrotasks();
            Timer.periodic(elapseBy ~/ 5, (_) {
              timerCalls++;
              expect(microtaskCalls, 5 * timerCalls);
              scheduleMicrotasks();
            });
            fakeAsync.elapse(elapseBy);
            expect(timerCalls, 5);
            expect(microtaskCalls, 5 * (timerCalls + 1));
          });
        });

        test('should pass the periodic timer itself to callbacks', () {
          FakeAsync().run((FakeAsync fakeAsync) {
            Timer passedTimer;
            final Timer periodic = Timer.periodic(elapseBy, (Timer timer) {
              passedTimer = timer;
            });
            fakeAsync.elapse(elapseBy);
            expect(periodic, same(passedTimer));
          });
        });

        test('should call microtasks before advancing time', () {
          FakeAsync().run((FakeAsync fakeAsync) {
            DateTime calledAt;
            scheduleMicrotask(() {
              calledAt = fakeAsync.getClock(initialTime).now();
            });
            fakeAsync.elapse(const Duration(minutes: 1));
            expect(calledAt, initialTime);
          });
        });

        test('should add event before advancing time', () {
          return Future<dynamic>(() => FakeAsync().run((FakeAsync fakeAsync) {
            final StreamController<void> controller = StreamController<void>();
              final Future<void> result = controller.stream.first.then((_) {
                expect(fakeAsync.getClock(initialTime).now(), initialTime);
              });
              controller.add(null);
              fakeAsync.elapse(const Duration(minutes: 1));
              return result;
            }));
        });

        test('should increase negative duration timers to zero duration', () {
          FakeAsync().run((FakeAsync fakeAsync) {
            const Duration negativeDuration = Duration(days: -1);
            DateTime calledAt;
            Timer(negativeDuration, () {
              calledAt = fakeAsync.getClock(initialTime).now();
            });
            fakeAsync.elapse(const Duration(minutes: 1));
            expect(calledAt, initialTime);
          });
        });

        test('should not be additive with elapseBlocking', () {
          FakeAsync().run((FakeAsync fakeAsync) {
            Timer(Duration.zero, () => fakeAsync.elapseBlocking(elapseBy * 5));
            fakeAsync.elapse(elapseBy);
            expect(fakeAsync.getClock(initialTime).now(),
                initialTime.add(elapseBy * 5));
          });
        });

        group('isActive', () {
          test('should be false after timer is run', () {
            FakeAsync().run((FakeAsync fakeAsync) {
              final Timer timer = Timer(elapseBy ~/ 2, () {});
              fakeAsync.elapse(elapseBy);
              expect(timer.isActive, isFalse);
            });
          });

          test('should be true after periodic timer is run', () {
            FakeAsync().run((FakeAsync fakeAsync) {
              final Timer timer = Timer.periodic(elapseBy ~/ 2, (_) {});
              fakeAsync.elapse(elapseBy);
              expect(timer.isActive, isTrue);
            });
          });

          test('should be false after timer is canceled', () {
            FakeAsync().run((FakeAsync fakeAsync) {
              final Timer timer = Timer(elapseBy ~/ 2, () {});
              timer.cancel();
              expect(timer.isActive, isFalse);
            });
          });
        });

        test('should work with Future()', () {
          FakeAsync().run((FakeAsync fakeAsync) {
            int callCount = 0;
            Future<int>(() => callCount++);
            fakeAsync.elapse(Duration.zero);
            expect(callCount, 1);
          });
        });

        test('should work with Future.delayed', () {
          FakeAsync().run((FakeAsync fakeAsync) {
            int result;
            Future<void>.delayed(elapseBy, () => result = 5);
            fakeAsync.elapse(elapseBy);
            expect(result, 5);
          });
        });

        test('should work with Future.timeout', () {
          FakeAsync().run((FakeAsync fakeAsync) {
            final Completer<void> completer = Completer<void>();
            TimeoutException timeout;
            completer.future.timeout(elapseBy ~/ 2).catchError((dynamic err) {
              timeout = err;
            });
            fakeAsync.elapse(elapseBy);
            expect(timeout, isInstanceOf<TimeoutException>());
            completer.complete();
          });
        });

        // TODO: Pausing and resuming the timeout Stream doesn't work since
        // it uses `Stopwatch()`.
        //
        // See https://code.google.com/p/dart/issues/detail?id=18149
        test('should work with Stream.periodic', () {
          FakeAsync().run((FakeAsync fakeAsync) {
            final List<int> events = <int>[];
            StreamSubscription<int> subscription;
            final Stream<int> periodic = Stream<int>.periodic(const Duration(minutes: 1), (int i) => i);
            subscription = periodic.listen(events.add);
            fakeAsync.elapse(const Duration(minutes: 3));
            expect(events, <int>[0, 1, 2]);
            subscription.cancel();
          });
        });

        test('should work with Stream.timeout', () {
          FakeAsync().run((FakeAsync fakeAsync) {
            final List<int> events = <int>[];
            final List<dynamic> errors = <dynamic>[];
            final StreamController<int> controller = StreamController<int>();
            final Stream<int> timed = controller.stream.timeout(const Duration(minutes: 2));
            final StreamSubscription<int> subscription = timed.listen(events.add, onError: errors.add);
            controller.add(0);
            fakeAsync.elapse(const Duration(minutes: 1));
            expect(events, <int>[0]);
            fakeAsync.elapse(const Duration(minutes: 1));
            expect(errors, hasLength(1));
            expect(errors.first, isInstanceOf<TimeoutException>());
            subscription.cancel();
            controller.close();
          });
        });
      });
    });

    group('flushMicrotasks', () {
      test('should flush a microtask', () {
        FakeAsync().run((FakeAsync fakeAsync) {
          bool microtaskRan = false;
          Future<void>.microtask(() {
            microtaskRan = true;
          });
          expect(microtaskRan, isFalse, reason: 'should not flush until asked to');
          fakeAsync.flushMicrotasks();
          expect(microtaskRan, isTrue);
        });
      });
      test('should flush microtasks scheduled by microtasks in order', () {
        FakeAsync().run((FakeAsync fakeAsync) {
          final List<int> log = <int>[];
          Future<void>.microtask(() {
            log.add(1);
            Future<void>.microtask(() {
              log.add(3);
            });
          });
          Future<void>.microtask(() {
            log.add(2);
          });
          expect(log, hasLength(0), reason: 'should not flush until asked to');
          fakeAsync.flushMicrotasks();
          expect(log, <int>[1, 2, 3]);
        });
      });
      test('should not run timers', () {
        FakeAsync().run((FakeAsync fakeAsync) {
          final List<int> log = <int>[];
          Future<void>.microtask(() {
            log.add(1);
          });
          Future<void>(() {
            log.add(2);
          });
          Timer.periodic(const Duration(seconds: 1), (_) {
            log.add(2);
          });
          fakeAsync.flushMicrotasks();
          expect(log, <int>[1]);
        });
      });
    });

    group('flushTimers', () {
      test('should flush timers in FIFO order', () {
        FakeAsync().run((FakeAsync fakeAsync) {
          final List<int> log = <int>[];
          Future<void>(() {
            log.add(1);
            Future<void>.delayed(elapseBy, () {
              log.add(3);
            });
          });
          Future<void>(() {
            log.add(2);
          });
          expect(log, hasLength(0), reason: 'should not flush until asked to');
          fakeAsync.flushTimers(timeout: elapseBy * 2, flushPeriodicTimers: false);
          expect(log, <int>[1, 2, 3]);
          expect(fakeAsync.getClock(initialTime).now(), initialTime.add(elapseBy));
        });
      });

      test(
          'should run collateral periodic timers with non-periodic first if '
          'scheduled first', () {
        FakeAsync().run((FakeAsync fakeAsync) {
          final List<String> log = <String>[];
          Future<void>.delayed(const Duration(seconds: 2), () {
            log.add('delayed');
          });
          Timer.periodic(const Duration(seconds: 1), (_) {
            log.add('periodic');
          });
          expect(log, hasLength(0), reason: 'should not flush until asked to');
          fakeAsync.flushTimers(flushPeriodicTimers: false);
          expect(log, <String>['periodic', 'delayed', 'periodic']);
        });
      });

      test(
          'should run collateral periodic timers with periodic first '
          'if scheduled first', () {
        FakeAsync().run((FakeAsync fakeAsync) {
          final List<String> log = <String>[];
          Timer.periodic(const Duration(seconds: 1), (_) {
            log.add('periodic');
          });
          Future<void>.delayed(const Duration(seconds: 2), () {
            log.add('delayed');
          });
          expect(log, hasLength(0), reason: 'should not flush until asked to');
          fakeAsync.flushTimers(flushPeriodicTimers: false);
          expect(log, <String>['periodic', 'periodic', 'delayed']);
        });
      });

      test('should timeout', () {
        FakeAsync().run((FakeAsync fakeAsync) {
          int count = 0;
          // Schedule 3 timers. All but the last one should fire.
          for (int delay in <int>[30, 60, 90]) {
            Future<void>.delayed(Duration(minutes: delay), () {
              count++;
            });
          }
          expect(() => fakeAsync.flushTimers(flushPeriodicTimers: false), throwsA(isInstanceOf<AssertionError>()));
          expect(count, 2);
        });
      });

      test('should timeout a chain of timers', () {
        FakeAsync().run((FakeAsync fakeAsync) {
          int count = 0;
          void createTimer() {
            Future<void>.delayed(const Duration(minutes: 30), () {
              count++;
              createTimer();
            });
          }
          createTimer();
          expect(
            () => fakeAsync.flushTimers(timeout: const Duration(hours: 2), flushPeriodicTimers: false),
            throwsA(isInstanceOf<AssertionError>()),
          );
          expect(count, 4);
        });
      });

      test('should timeout periodic timers', () {
        FakeAsync().run((FakeAsync fakeAsync) {
          int count = 0;
          Timer.periodic(const Duration(minutes: 30), (Timer timer) {
            count++;
          });
          expect(() => fakeAsync.flushTimers(timeout: const Duration(hours: 1)), throwsA(isInstanceOf<AssertionError>()));
          expect(count, 2);
        });
      });

      test('should flush periodic timers', () {
        FakeAsync().run((FakeAsync fakeAsync) {
          int count = 0;
          Timer.periodic(const Duration(minutes: 30), (Timer timer) {
            if (count == 3) {
              timer.cancel();
            }
            count++;
          });
          fakeAsync.flushTimers(timeout: const Duration(hours: 20));
          expect(count, 4);
        });
      });

      test('should compute absolute timeout as elapsed + timeout', () {
        FakeAsync().run((FakeAsync fakeAsync) {
          final List<int> log = <int>[];
          int count = 0;
          void createTimer() {
            Future<void>.delayed(const Duration(minutes: 30), () {
              log.add(count);
              count++;
              if (count < 4) {
                createTimer();
              }
            });
          }
          createTimer();
          fakeAsync.elapse(const Duration(hours: 1));
          fakeAsync.flushTimers(timeout: const Duration(hours: 1));
          expect(count, 4);
        });
      });
    });

    group('stats', () {
      test('should report the number of pending microtasks', () {
        FakeAsync().run((FakeAsync fakeAsync) {
          expect(fakeAsync.microtaskCount, 0);
          scheduleMicrotask(() => null);
          expect(fakeAsync.microtaskCount, 1);
          scheduleMicrotask(() => null);
          expect(fakeAsync.microtaskCount, 2);
          fakeAsync.flushMicrotasks();
          expect(fakeAsync.microtaskCount, 0);
        });
      });

      test('it should report the number of pending periodic timers', () {
        FakeAsync().run((FakeAsync fakeAsync) {
          expect(fakeAsync.periodicTimerCount, 0);
          final Timer timer = Timer.periodic(const Duration(minutes: 30), (Timer timer) {});
          expect(fakeAsync.periodicTimerCount, 1);
          Timer.periodic(const Duration(minutes: 20), (Timer timer) {});
          expect(fakeAsync.periodicTimerCount, 2);
          fakeAsync.elapse(const Duration(minutes: 20));
          expect(fakeAsync.periodicTimerCount, 2);
          timer.cancel();
          expect(fakeAsync.periodicTimerCount, 1);
        });
      });

      test('it should report the number of pending non periodic timers', () {
        FakeAsync().run((FakeAsync fakeAsync) {
          expect(fakeAsync.nonPeriodicTimerCount, 0);
          final Timer timer = Timer(const Duration(minutes: 30), () {});
          expect(fakeAsync.nonPeriodicTimerCount, 1);
          Timer(const Duration(minutes: 20), () {});
          expect(fakeAsync.nonPeriodicTimerCount, 2);
          fakeAsync.elapse(const Duration(minutes: 25));
          expect(fakeAsync.nonPeriodicTimerCount, 1);
          timer.cancel();
          expect(fakeAsync.nonPeriodicTimerCount, 0);
        });
      });
    });

    group('timers', () {
      test('should behave like real timers', () {
        return FakeAsync().run((FakeAsync fakeAsync) {
          const Duration timeout = Duration(minutes: 1);
          int counter = 0;
          Timer timer;
          timer = Timer(timeout, () {
            counter++;
            expect(timer.isActive, isFalse, reason: 'is not active while executing callback');
          });
          expect(timer.isActive, isTrue, reason: 'is active before executing callback');
          fakeAsync.elapse(timeout);
          expect(counter, equals(1), reason: 'timer executed');
          expect(timer.isActive, isFalse, reason: 'is not active after executing callback');
        });
      });
    });
  });
}