package dev.featherdart

/**
 * Exception thrown by the FeatherDart Kotlin embedding layer on VM initialization,
 * snapshot loading, FFI symbol resolution, or uncaught synchronous Dart errors.
 */
class FeatherDartException(message: String, cause: Throwable? = null) :
    RuntimeException(message, cause)
