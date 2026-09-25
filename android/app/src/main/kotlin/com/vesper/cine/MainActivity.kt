package com.vesper.cine

import android.Manifest
import android.content.ContentValues
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Bundle
import android.provider.MediaStore
import android.view.Surface
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel
import io.flutter.view.TextureRegistry
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

class MainActivity : FlutterActivity() {
    private val channelName = "com.vesper.cine/native"
    private var producer: TextureRegistry.SurfaceProducer? = null

    companion object {
        init {
            System.loadLibrary("vesper_engine")
        }
    }

    private external fun nativeSetViewfinderSurface(surface: Surface?): Int

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val missing = arrayOf(Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO)
            .filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
        if (missing.isNotEmpty()) requestPermissions(missing.toTypedArray(), 1001)
    }

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, channelName).setMethodCallHandler { call, result ->
            when (call.method) {
                "createTexture" -> {
                    val width = call.argument<Int>("width") ?: 1920
                    val height = call.argument<Int>("height") ?: 1080
                    val entry = producer ?: flutterEngine.renderer.createSurfaceProducer().also { p ->
                        // Flutter may tear the Surface down (e.g. app backgrounded) and
                        // hand out a new one later; the GPU engine must follow it.
                        p.setCallback(object : TextureRegistry.SurfaceProducer.Callback {
                            override fun onSurfaceAvailable() {
                                nativeSetViewfinderSurface(p.surface)
                            }

                            override fun onSurfaceCleanup() {
                                nativeSetViewfinderSurface(null)
                            }
                        })
                        producer = p
                    }
                    entry.setSize(width, height)
                    nativeSetViewfinderSurface(entry.surface)
                    result.success(entry.id())
                }
                "resizeTexture" -> {
                    val entry = producer
                    if (entry == null) {
                        result.success(false)
                    } else {
                        entry.setSize(call.argument<Int>("width") ?: 1920, call.argument<Int>("height") ?: 1080)
                        nativeSetViewfinderSurface(entry.surface)
                        result.success(true)
                    }
                }
                "destroyTexture" -> {
                    releaseTexture()
                    result.success(true)
                }
                "createRecordingFile" -> {
                    try {
                        result.success(createRecordingFile())
                    } catch (e: Exception) {
                        result.error("file", e.message, null)
                    }
                }
                "finalizeRecordingFile" -> {
                    val uri = Uri.parse(call.argument<String>("uri"))
                    val keep = call.argument<Boolean>("keep") ?: true
                    try {
                        if (keep) {
                            val values = ContentValues().apply { put(MediaStore.Video.Media.IS_PENDING, 0) }
                            contentResolver.update(uri, values, null, null)
                        } else {
                            contentResolver.delete(uri, null, null)
                        }
                        result.success(true)
                    } catch (e: Exception) {
                        result.error("file", e.message, null)
                    }
                }
                else -> result.notImplemented()
            }
        }
    }

    // Creates Movies/Vesper Cine/VESPER_<timestamp>.mp4 through MediaStore
    // (no storage permission needed) and hands the native recorder a raw fd.
    private fun createRecordingFile(): Map<String, Any> {
        val name = "VESPER_" + SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date()) + ".mp4"
        val values = ContentValues().apply {
            put(MediaStore.Video.Media.DISPLAY_NAME, name)
            put(MediaStore.Video.Media.MIME_TYPE, "video/mp4")
            put(MediaStore.Video.Media.RELATIVE_PATH, "Movies/Vesper Cine")
            put(MediaStore.Video.Media.IS_PENDING, 1)
        }
        val collection = MediaStore.Video.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY)
        val uri = contentResolver.insert(collection, values) ?: throw IllegalStateException("MediaStore insert failed")
        val pfd = contentResolver.openFileDescriptor(uri, "rw") ?: throw IllegalStateException("open failed")
        return mapOf("fd" to pfd.detachFd(), "uri" to uri.toString(), "name" to name)
    }

    private fun releaseTexture() {
        nativeSetViewfinderSurface(null)
        producer?.release()
        producer = null
    }

    override fun onDestroy() {
        releaseTexture()
        super.onDestroy()
    }
}
