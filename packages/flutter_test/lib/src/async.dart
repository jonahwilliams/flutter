// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';

/// Return current system time from [DateTime.now].
DateTime systemTime() => DateTime.now();

/// A function which returns the current time.
typedef TimeFunction = DateTime Function();

/// Provides points in time relative to the current point in time, for example:
/// now, 2 days ago, 4 weeks from now, etc.
///
/// This class is designed with testability in mind. The current point in time
/// (or [now()]) is defined by a [TimeFunction]. By supplying your own time
/// function or by using fixed clock (see constructors), you can control
/// exactly what time a [Clock] returns and base your test expectations on
/// that. See specific constructors for how to supply time functions.
class Clock {
  /// Creates a [Clock].
  ///
  /// An optional [TimeFunction] may be supplied to control the passage of
  /// time. If not provided, defaults to [systemTime].
  const Clock([this._timeFunction = systemTime]);

  final TimeFunction _timeFunction;

  /// The current time.
  DateTime now() => _timeFunction();

  /// Returns the point in time [Duration] amount of time from now.
  DateTime fromNowBy(Duration duration) => now().add(duration);
}

/// A mechanism to make time-dependent units testable.
///
/// Test code can be passed as a callback to [run], which causes it to be run in
/// a [Zone] which fakes timer and microtask creation, such that they are run
/// during calls to [elapse] which simulates the asynchronous passage of time.
///
/// The synchronous passage of time (blocking or expensive calls) can also be
/// simulated using [elapseBlocking].
///
/// To allow the unit under test to tell time, it can receive a [Clock] as a
/// dependency, and default it to [const Clock()] in production, but then use
/// [clock] in test code.
///
/// Example:
///
///     test('testedFunc', () {
///       new FakeAsync().run((async) {
///         testedFunc(clock: async.getClock(initialTime));
///         async.elapse(duration);
///         expect(...)
///       });
///     });
abstract class FakeAsync {
  /// Create a new [FakeAsync] instance.
  factory FakeAsync() = _FakeAsync;

  /// Returns a fake [Clock] whose time can is elapsed by calls to [elapse] and
  /// [elapseBlocking].
  ///
  /// The returned clock starts at [initialTime], and calls to [elapse] and
  /// [elapseBlocking] advance the clock, even if they occured before the call
  /// to this method.
  ///
  /// The clock can be passed as a dependency to the unit under test.
  Clock getClock(DateTime initialTime);

  /// Simulates the asynchronous passage of time.
  ///
  /// **This should only be called from within the zone used by [run].**
  ///
  /// If [duration] is negative, the returned future completes with an
  /// [ArgumentError].
  ///
  /// If a previous call to [elapse] has not yet completed, throws a
  /// [StateError].
  ///
  /// Any Timers created within the zone used by [run] which are to expire
  /// at or before the new time after [duration] has elapsed are run.
  /// The microtask queue is processed surrounding each timer.  When a timer is
  /// run, the [clock] will have been advanced by the timer's specified
  /// duration.  Calls to [elapseBlocking] from within these timers and
  /// microtasks which cause the [clock] to elapse more than the specified
  /// [duration], can cause more timers to expire and thus be called.
  ///
  /// Once all expired timers are processed, the [clock] is advanced (if
  /// necessary) to the time this method was called + [duration].
  void elapse(Duration duration);

  /// Simulates the synchronous passage of time, resulting from blocking or
  /// expensive calls.
  ///
  /// Neither timers nor microtasks are run during this call.  Upon return, the
  /// [clock] will have been advanced by [duration].
  ///
  /// If [duration] is negative, throws an [ArgumentError].
  void elapseBlocking(Duration duration);

  /// Runs [callback] in a [Zone] with fake timer and microtask scheduling.
  ///
  /// Uses
  /// [ZoneSpecification.createTimer], [ZoneSpecification.createPeriodicTimer],
  /// and [ZoneSpecification.scheduleMicrotask] to store callbacks for later
  /// execution within the zone via calls to [elapse].
  ///
  /// Calls [callback] with `this` as argument and returns the result returned
  /// by [callback].
  dynamic run(dynamic Function(FakeAsync) callback);

  /// Runs all remaining microtasks, including those scheduled as a result of
  /// running them, until there are no more microtasks scheduled.
  ///
  /// Does not run timers.
  void flushMicrotasks();

  /// Runs all timers until no timers remain (subject to [flushPeriodicTimers]
  /// option), including those scheduled as a result of running them.
  ///
  /// [timeout] lets you set the maximum amount of time the flushing will take.
  /// Throws a [StateError] if the [timeout] is exceeded. The default timeout
  /// is 1 hour. [timeout] is relative to the elapsed time.
  void flushTimers({Duration timeout = const Duration(hours: 1), bool flushPeriodicTimers = true});

  /// The number of created periodic timers that have not been canceled.
  int get periodicTimerCount;

  /// The number of pending non periodic timers that have not been canceled.
  int get nonPeriodicTimerCount;

  /// The number of pending microtasks.
  int get microtaskCount;
}

class _FakeAsync implements FakeAsync {
  Duration _elapsed = Duration.zero;
  Duration _elapsingTo;
  final List<Function> _microtasks = <Function>[];
  final Set<_FakeTimer> _timers = Set<_FakeTimer>();

  @override
  Clock getClock(DateTime initialTime) => Clock(() => initialTime.add(_elapsed));

  @override
  void elapse(Duration duration) {
    assert(duration.inMicroseconds >= 0, 'Cannot call elapse with negative duration');
    assert(_elapsingTo == null, 'Cannot elapse until previous elapse is complete.');
    _elapsingTo = _elapsed + duration;
    _drainTimersWhile((_FakeTimer next) => next._nextCall <= _elapsingTo);
    _elapseTo(_elapsingTo);
    _elapsingTo = null;
  }

  @override
  void elapseBlocking(Duration duration) {
    assert(duration.inMicroseconds >= 0, 'Cannot call elapse with negative duration');
    _elapsed += duration;
    if (_elapsingTo != null && _elapsed > _elapsingTo) {
      _elapsingTo = _elapsed;
    }
  }

  @override
  void flushMicrotasks() {
    _drainMicrotasks();
  }

  @override
  void flushTimers({Duration timeout = const Duration(hours: 1), bool flushPeriodicTimers = true}) {
    final Duration absoluteTimeout = _elapsed + timeout;
    _drainTimersWhile((_FakeTimer timer) {
      assert(timer._nextCall <= absoluteTimeout, 'Exceeded timeout $timeout while flushing timers');
      if (flushPeriodicTimers) {
        return _timers.isNotEmpty;
      } else {
        // drain every timer (periodic or not) that will occur up
        // until the latest non-periodic timer
        return _timers.any((_FakeTimer timer) =>
            !timer._isPeriodic || timer._nextCall <= _elapsed);
      }
    });
  }

  @override
  dynamic run(dynamic Function(FakeAsync) callback) {
    _zone ??= Zone.current.fork(specification: _zoneSpec);
    dynamic result;
    _zone.runGuarded(() {
      result = callback(this);
    });
    return result;
  }

  Zone _zone;

  @override
  int get periodicTimerCount =>
      _timers.where((_FakeTimer timer) => timer._isPeriodic).length;

  @override
  int get nonPeriodicTimerCount =>
      _timers.where((_FakeTimer timer) => !timer._isPeriodic).length;

  @override
  int get microtaskCount => _microtasks.length;

  ZoneSpecification get _zoneSpec => ZoneSpecification(
          createTimer: (_, __, ___, Duration duration, Function callback) {
        return _createTimer(duration, callback, false);
      }, createPeriodicTimer:
              (_, __, ___, Duration duration, Function callback) {
        return _createTimer(duration, callback, true);
      }, scheduleMicrotask: (_, __, ___, Function microtask) {
        _microtasks.add(microtask);
      });

  void _drainTimersWhile(bool predicate(_FakeTimer timer)) {
    _drainMicrotasks();
    _FakeTimer next;
    while ((next = _getNextTimer()) != null && predicate(next)) {
      _runTimer(next);
      _drainMicrotasks();
    }
  }

  void _elapseTo(Duration to) {
    if (to > _elapsed) {
      _elapsed = to;
    }
  }

  Timer _createTimer(Duration duration, Function callback, bool isPeriodic) {
    final _FakeTimer timer = _FakeTimer._(duration, callback, isPeriodic, this);
    _timers.add(timer);
    return timer;
  }

  _FakeTimer _getNextTimer() {
    return _timers.isEmpty
        ? null
        : _timers.reduce((_FakeTimer t1, _FakeTimer t2) => t1._nextCall <= t2._nextCall ? t1 : t2);
  }

  void _runTimer(_FakeTimer timer) {
    assert(timer.isActive);
    _elapseTo(timer._nextCall);
    if (timer._isPeriodic) {
      timer._callback(timer);
      timer._nextCall += timer._duration;
    } else {
      _timers.remove(timer);
      timer._callback();
    }
  }

  void _drainMicrotasks() {
    while (_microtasks.isNotEmpty) {
      _microtasks.removeAt(0)();
    }
  }

  bool _hasTimer(_FakeTimer timer) => _timers.contains(timer);

  void _cancelTimer(_FakeTimer timer) => _timers.remove(timer);
}

class _FakeTimer implements Timer {
  _FakeTimer._(Duration duration, this._callback, this._isPeriodic, this._time)
    : _duration = duration < _minDuration ? _minDuration : duration {
    _nextCall = _time._elapsed + _duration;
  }

  final Duration _duration;
  final Function _callback;
  final bool _isPeriodic;
  final _FakeAsync _time;
  Duration _nextCall;
  static const Duration _minDuration = Duration.zero;

  @override
  bool get isActive => _time._hasTimer(this);

  @override
  void cancel() => _time._cancelTimer(this);

  @override
  int get tick {
    throw UnimplementedError();
  }
}