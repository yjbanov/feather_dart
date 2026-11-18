library dart._feather_dart;

import 'dart:_internal' show VMLibraryHooks;
import 'dart:ffi' show Handle, Int64, Native, NativePort, Void;
import 'dart:isolate' show SendPort;

@Native<Void Function(Handle)>(symbol: 'feather_dart_print_handle')
external void _nativePrintHandle(Object line);

@Native<Int64 Function()>(symbol: 'feather_dart_monotonic_ms', isLeaf: true)
external int _nativeMonotonicMillis();

@Native<Void Function(Int64, Int64)>(
  symbol: 'feather_dart_schedule_wakeup',
  isLeaf: true,
)
external void _nativeScheduleWakeup(int portId, int wakeupTimeMs);

void _printToHost(String line) {
  _nativePrintHandle(line);
}

void _sendTimerWakeup(Object? sender, SendPort sendPort, int wakeupTimeMs) {
  _nativeScheduleWakeup(sendPort.nativePort, wakeupTimeMs);
}

@pragma('vm:entry-point', 'call')
void _setupFeatherDartHooks() {
  VMLibraryHooks.timerMillisecondClock = _nativeMonotonicMillis;
  VMLibraryHooks.eventHandlerSendData = _sendTimerWakeup;
}

@pragma('vm:entry-point', 'call')
void Function(String) _getPrintClosure() => _printToHost;
