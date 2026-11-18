package dev.featherdart

import org.junit.Assert.assertEquals
import org.junit.Test

class FeatherDartConfigTest {
    @Test
    fun exceptionPreservesMessage() {
        val ex = FeatherDartException("missing symbol: fd_compute")
        assertEquals("missing symbol: fd_compute", ex.message)
    }
}
