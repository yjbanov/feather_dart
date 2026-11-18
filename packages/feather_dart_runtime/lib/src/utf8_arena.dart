import 'dart:convert';
import 'dart:ffi';
import 'dart:typed_data';

@Native<Pointer<Void> Function(IntPtr)>(symbol: 'malloc', isLeaf: true)
external Pointer<Void> _posixMalloc(int size);

@Native<Void Function(Pointer<Void>)>(symbol: 'free', isLeaf: true)
external void _posixFree(Pointer<Void> ptr);

/// Scoped native memory arena backed by libc `malloc`/`free`.
///
/// Requires zero external packages and works identically on iOS, Android,
/// macOS, and Linux.
final class NativeArena {
  final List<Pointer<Void>> _allocations = <Pointer<Void>>[];
  bool _released = false;

  /// Runs [action] with a temporary [NativeArena] and frees all allocations
  /// when [action] returns or throws.
  static R using<R>(R Function(NativeArena arena) action) {
    final arena = NativeArena();
    try {
      return action(arena);
    } finally {
      arena.releaseAll();
    }
  }

  /// Allocates [byteCount] uninitialized bytes on the native heap.
  Pointer<Uint8> allocateBytes(int byteCount) {
    if (_released) {
      throw StateError('NativeArena has already been released.');
    }
    if (byteCount < 0) {
      throw RangeError.value(byteCount, 'byteCount');
    }
    final actualSize = byteCount == 0 ? 1 : byteCount;
    final ptr = _posixMalloc(actualSize);
    if (ptr == nullptr) {
      throw OutOfMemoryError();
    }
    _allocations.add(ptr);
    return ptr.cast<Uint8>();
  }

  /// Allocates a null-terminated UTF-8 C string in this arena.
  Pointer<Uint8> toCString(String value) {
    final encoded = utf8.encode(value);
    final buf = allocateBytes(encoded.length + 1);
    final view = buf.asTypedList(encoded.length + 1);
    view.setRange(0, encoded.length, encoded);
    view[encoded.length] = 0;
    return buf;
  }

  /// Allocates a native `Float64` buffer of [length] elements.
  Pointer<Double> allocateFloat64(int length) {
    return allocateBytes(length * sizeOf<Double>()).cast<Double>();
  }

  /// Releases all allocations owned by this arena.
  void releaseAll() {
    if (_released) {
      return;
    }
    _released = true;
    for (var i = _allocations.length - 1; i >= 0; i--) {
      _posixFree(_allocations[i]);
    }
    _allocations.clear();
  }
}

/// Zero-dependency UTF-8 and byte-buffer helpers for Direct FFI boundaries.
abstract final class FfiUtf8 {
  /// Decodes a UTF-8 string from [ptr]. If [byteLength] is omitted, scans for
  /// a trailing NUL byte.
  static String fromPointer(Pointer<Uint8> ptr, {int? byteLength}) {
    if (ptr == nullptr) {
      throw ArgumentError.notNull('ptr');
    }
    int len;
    if (byteLength != null) {
      if (byteLength < 0) {
        throw RangeError.value(byteLength, 'byteLength');
      }
      len = byteLength;
    } else {
      len = 0;
      while (Pointer<Uint8>.fromAddress(ptr.address + len).value != 0) {
        len++;
      }
    }
    if (len == 0) {
      return '';
    }
    return utf8.decode(ptr.asTypedList(len));
  }

  /// Encodes [value] as UTF-8 into caller-provided `[outBuffer, outCapacity)`.
  ///
  /// - If `outBuffer == nullptr` or `outCapacity < encoded.length`, writes
  ///   nothing and returns `-encoded.length` (allowing the caller to retry with
  ///   an exact-sized buffer).
  /// - Otherwise copies the UTF-8 bytes (and trailing NUL if space permits) and
  ///   returns the number of UTF-8 bytes written (excluding NUL).
  static int writeInto(
    String value,
    Pointer<Uint8> outBuffer,
    int outCapacity,
  ) {
    final Uint8List encoded = utf8.encode(value);
    if (encoded.isEmpty) {
      if (outBuffer != nullptr && outCapacity > 0) {
        outBuffer.value = 0;
      }
      return 0;
    }
    if (outBuffer == nullptr || outCapacity < encoded.length) {
      return -encoded.length;
    }
    final view = outBuffer.asTypedList(outCapacity);
    view.setRange(0, encoded.length, encoded);
    if (outCapacity > encoded.length) {
      view[encoded.length] = 0;
    }
    return encoded.length;
  }
}
