package com.rawedge.r_camera

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import android.view.Surface
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel
import io.flutter.view.TextureRegistry

class MainActivity : FlutterActivity() {
    private val CHANNEL = "com.rawedge.r_camera/texture"
    private var surfaceProducer: TextureRegistry.SurfaceProducer? = null
    private var surfaceTextureEntry: TextureRegistry.SurfaceTextureEntry? = null
    private var activeSurface: Surface? = null

    companion object {
        init {
            System.loadLibrary("rcamera_engine")
        }
    }

    private external fun nativeSetViewfinderSurface(surface: Surface?): Int

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val permissions = arrayOf(
            Manifest.permission.CAMERA,
            Manifest.permission.RECORD_AUDIO
        )
        val missingPermissions = permissions.filter {
            checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED
        }
        if (missingPermissions.isNotEmpty()) {
            requestPermissions(missingPermissions.toTypedArray(), 1001)
        }
    }

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)

        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, CHANNEL).setMethodCallHandler { call, result ->
            when (call.method) {
                "createTexture" -> {
                    val width = call.argument<Int>("width") ?: 1920
                    val height = call.argument<Int>("height") ?: 1080
                    
                    try {
                        // Use SurfaceProducer if available (Flutter 3.7+), fallback to SurfaceTextureEntry
                        val entry = flutterEngine.renderer.createSurfaceProducer()
                        surfaceProducer = entry
                        entry.setSize(width, height)
                        val surface = entry.surface
                        activeSurface = surface
                        nativeSetViewfinderSurface(surface)
                        result.success(entry.id())
                    } catch (e: Throwable) {
                        val entry = flutterEngine.renderer.createSurfaceTexture()
                        surfaceTextureEntry = entry
                        entry.surfaceTexture().setDefaultBufferSize(width, height)
                        val surface = Surface(entry.surfaceTexture())
                        activeSurface = surface
                        nativeSetViewfinderSurface(surface)
                        result.success(entry.id())
                    }
                }
                "destroyTexture" -> {
                    nativeSetViewfinderSurface(null)
                    activeSurface?.release()
                    activeSurface = null
                    surfaceProducer?.release()
                    surfaceProducer = null
                    surfaceTextureEntry?.release()
                    surfaceTextureEntry = null
                    result.success(true)
                }
                else -> result.notImplemented()
            }
        }
    }

    override fun onDestroy() {
        nativeSetViewfinderSurface(null)
        activeSurface?.release()
        activeSurface = null
        surfaceProducer?.release()
        surfaceProducer = null
        surfaceTextureEntry?.release()
        surfaceTextureEntry = null
        super.onDestroy()
    }
}
