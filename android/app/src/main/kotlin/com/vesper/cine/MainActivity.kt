package com.vesper.cine

import android.Manifest
import android.content.ContentValues
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.hardware.display.DisplayManager
import android.net.Uri
import android.os.Build
import android.provider.MediaStore
import android.graphics.Color
import android.graphics.drawable.ColorDrawable
import android.os.Bundle
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.ViewGroup
import android.widget.FrameLayout
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.android.RenderMode
import io.flutter.embedding.android.TransparencyMode
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

class MainActivity : FlutterActivity() {
    private val channelName = "com.vesper.cine/native"

    // The viewfinder is a plain SurfaceView behind the (transparent) Flutter
    // UI: the GPU engine presents straight to the system compositor, so
    // preview frames never wait for a Flutter frame (that path showed up as
    // 85-100 ms gaps). Flutter reports where to put it (setViewfinderRect).
    private var vfView: SurfaceView? = null
    private var vfWanted = false // Dart created the viewfinder

    private val vfCallback = object : SurfaceHolder.Callback {
        override fun surfaceCreated(holder: SurfaceHolder) {
            if (vfWanted) nativeSetViewfinderSurface(holder.surface)
        }

        override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
            if (vfWanted) nativeSetViewfinderSurface(holder.surface)
        }

        override fun surfaceDestroyed(holder: SurfaceHolder) {
            nativeSetViewfinderSurface(null)
        }
    }

    // Flutter draws on its own surface above everything, transparently where
    // the viewfinder is; the window underneath is black.
    override fun getRenderMode(): RenderMode = RenderMode.surface
    override fun getTransparencyMode(): TransparencyMode = TransparencyMode.transparent

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.setBackgroundDrawable(ColorDrawable(Color.BLACK))
        val view = SurfaceView(this)
        view.holder.addCallback(vfCallback)
        findViewById<ViewGroup>(android.R.id.content).addView(view, 0, FrameLayout.LayoutParams(1, 1))
        vfView = view
    }

    private fun attachViewfinder(width: Int, height: Int) {
        val holder = vfView?.holder ?: return
        vfWanted = true
        holder.setFixedSize(width, height) // buffer = output size: copied 1:1, scaled by the compositor
        if (holder.surface?.isValid == true) nativeSetViewfinderSurface(holder.surface)
    }

    companion object {
        init {
            System.loadLibrary("vesper_engine")
        }
    }

    private external fun nativeSetViewfinderSurface(surface: Surface?): Int
    private external fun nativeSetDisplayRotation(degrees: Int)

    // The UI runs in both landscape orientations; the native pipeline needs to
    // know which one so the picture stays upright. A 180-degree flip between
    // the two landscapes is not a configuration change, so listen to the display.
    private val displayListener = object : DisplayManager.DisplayListener {
        override fun onDisplayAdded(displayId: Int) {}
        override fun onDisplayRemoved(displayId: Int) {}
        override fun onDisplayChanged(displayId: Int) = reportRotation()
    }

    private fun reportRotation() {
        val degrees = when (display?.rotation) {
            Surface.ROTATION_270 -> 270
            else -> 90
        }
        nativeSetDisplayRotation(degrees)
    }

    override fun onResume() {
        super.onResume()
        (getSystemService(DISPLAY_SERVICE) as DisplayManager).registerDisplayListener(displayListener, null)
        reportRotation()
    }

    override fun onPause() {
        (getSystemService(DISPLAY_SERVICE) as DisplayManager).unregisterDisplayListener(displayListener)
        super.onPause()
    }

    // Camera/microphone permission is requested from Dart (requestPermissions)
    // and the camera is only opened after the user has answered; opening it
    // before the grant failed silently on first launch.
    private var permissionResult: MethodChannel.Result? = null

    private fun missingPermissions() = arrayOf(Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO)
        .filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != 1001) return
        val r = permissionResult ?: return
        permissionResult = null
        r.success(missingPermissions().isEmpty())
    }

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, channelName).setMethodCallHandler { call, result ->
            when (call.method) {
                "createTexture" -> {
                    attachViewfinder(call.argument<Int>("width") ?: 1920, call.argument<Int>("height") ?: 1080)
                    result.success(0)
                }
                "resizeTexture" -> {
                    attachViewfinder(call.argument<Int>("width") ?: 1920, call.argument<Int>("height") ?: 1080)
                    result.success(true)
                }
                "destroyTexture" -> {
                    releaseTexture()
                    result.success(true)
                }
                "setViewfinderRect" -> {
                    val w = call.argument<Int>("width") ?: 1
                    val h = call.argument<Int>("height") ?: 1
                    vfView?.layoutParams = FrameLayout.LayoutParams(maxOf(w, 1), maxOf(h, 1)).apply {
                        leftMargin = call.argument<Int>("left") ?: 0
                        topMargin = call.argument<Int>("top") ?: 0
                    }
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
                "requestPermissions" -> {
                    // Resolves true once camera AND microphone are granted (audio is part of every recording).
                    val missing = missingPermissions()
                    if (missing.isEmpty()) {
                        result.success(true)
                    } else if (permissionResult != null) {
                        result.error("busy", "permission request already in progress", null)
                    } else {
                        permissionResult = result
                        requestPermissions(missing.toTypedArray(), 1001)
                    }
                }
                "lockRotation" -> {
                    // Freeze the current landscape while recording; free (both landscapes) otherwise.
                    requestedOrientation = if (call.argument<Boolean>("locked") == true)
                        ActivityInfo.SCREEN_ORIENTATION_LOCKED else ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
                    result.success(true)
                }
                "deviceInfo" -> {
                    val dir = java.io.File(getExternalFilesDir(null), "calibration").apply { mkdirs() }
                    result.success(mapOf("model" to Build.MODEL, "device" to Build.DEVICE, "calibrationDir" to dir.absolutePath, "filesDir" to filesDir.absolutePath))
                }
                "publishCalibration" -> {
                    val base = call.argument<String>("base") ?: ""
                    try {
                        for (ext in listOf(".raw10", ".json")) publishDownload(java.io.File(base + ext))
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

    // Copies an app-private calibration file to Downloads/Vesper Calibration so it
    // can be pulled off the phone without adb.
    private fun publishDownload(src: java.io.File) {
        val values = ContentValues().apply {
            put(MediaStore.Downloads.DISPLAY_NAME, src.name)
            put(MediaStore.Downloads.MIME_TYPE, if (src.name.endsWith(".json")) "application/json" else "application/octet-stream")
            put(MediaStore.Downloads.RELATIVE_PATH, "Download/Vesper Calibration")
        }
        val uri = contentResolver.insert(MediaStore.Downloads.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY), values)
            ?: throw IllegalStateException("MediaStore insert failed")
        contentResolver.openOutputStream(uri)!!.use { out -> src.inputStream().use { it.copyTo(out) } }
    }

    private fun releaseTexture() {
        vfWanted = false
        nativeSetViewfinderSurface(null)
    }

    override fun onDestroy() {
        releaseTexture()
        super.onDestroy()
    }
}
