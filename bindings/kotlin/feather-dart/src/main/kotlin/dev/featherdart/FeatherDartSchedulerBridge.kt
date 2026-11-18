package dev.featherdart

import android.os.Handler
import android.os.Looper
import java.util.concurrent.ConcurrentHashMap
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/**
 * Bridges Dart's asynchronous message loop and `Timer` wakeups onto an Android
 * [Handler]/[Looper] or a Kotlin [CoroutineScope] + [CoroutineDispatcher].
 */
interface FeatherDartSchedulerBridge {
    fun scheduleMessage(runMessage: () -> Unit)
    fun scheduleTimer(portId: Long, delayMs: Long, fireTimer: (Long) -> Unit)
    fun cancelAll()

    companion object {
        /**
         * Dispatches Dart async messages and `Timer` callbacks onto an Android
         * [Handler] (defaults to `Looper.getMainLooper()`).
         */
        fun androidHandler(
            handler: Handler = Handler(Looper.getMainLooper()),
        ): FeatherDartSchedulerBridge = AndroidHandlerSchedulerBridge(handler)

        /**
         * Dispatches Dart async messages and `Timer` callbacks onto a Kotlin
         * [CoroutineScope] using [dispatcher].
         */
        fun coroutines(
            scope: CoroutineScope,
            dispatcher: CoroutineDispatcher = Dispatchers.Default,
        ): FeatherDartSchedulerBridge = CoroutineSchedulerBridge(scope, dispatcher)
    }
}

private class AndroidHandlerSchedulerBridge(
    private val handler: Handler,
) : FeatherDartSchedulerBridge {
    private val pendingTimers = ConcurrentHashMap<Long, Runnable>()
    @Volatile private var cancelled = false

    override fun scheduleMessage(runMessage: () -> Unit) {
        if (cancelled) return
        handler.post {
            if (!cancelled) {
                runMessage()
            }
        }
    }

    override fun scheduleTimer(portId: Long, delayMs: Long, fireTimer: (Long) -> Unit) {
        pendingTimers.remove(portId)?.let { handler.removeCallbacks(it) }
        if (cancelled || delayMs < 0L) return

        val runnable = Runnable {
            pendingTimers.remove(portId)
            if (!cancelled) {
                fireTimer(portId)
            }
        }
        pendingTimers[portId] = runnable
        handler.postDelayed(runnable, delayMs.coerceAtLeast(0L))
    }

    override fun cancelAll() {
        cancelled = true
        for ((_, runnable) in pendingTimers) {
            handler.removeCallbacks(runnable)
        }
        pendingTimers.clear()
    }
}

private class CoroutineSchedulerBridge(
    private val scope: CoroutineScope,
    private val dispatcher: CoroutineDispatcher,
) : FeatherDartSchedulerBridge {
    private val timerJobs = ConcurrentHashMap<Long, Job>()
    @Volatile private var cancelled = false

    override fun scheduleMessage(runMessage: () -> Unit) {
        if (cancelled) return
        scope.launch(dispatcher) {
            if (!cancelled) {
                runMessage()
            }
        }
    }

    override fun scheduleTimer(portId: Long, delayMs: Long, fireTimer: (Long) -> Unit) {
        timerJobs.remove(portId)?.cancel()
        if (cancelled || delayMs < 0L) return

        val job = scope.launch(dispatcher) {
            if (delayMs > 0L) {
                delay(delayMs)
            }
            timerJobs.remove(portId)
            if (!cancelled) {
                fireTimer(portId)
            }
        }
        timerJobs[portId] = job
    }

    override fun cancelAll() {
        cancelled = true
        for ((_, job) in timerJobs) {
            job.cancel()
        }
        timerJobs.clear()
    }
}
