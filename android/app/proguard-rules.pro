# Extra R8 (code shrinking) rules for RELEASE builds. Flutter's Gradle plugin
# picks this file up automatically (FlutterPlugin.kt adds android/app/proguard-rules.pro).
#
# Why: on 13 Oct 2026 the first release build crashed at start-up. The Google ads
# library brings in WorkManager, which keeps a Room database (WorkDatabase) and
# creates it by reflection. R8 (full mode, AGP 9) removed the constructor it needs:
# "RuntimeException: Failed to create an instance of androidx.work.impl.WorkDatabase".
# Profile / debug builds skip R8, so only release was affected.

# Room databases are instantiated by name through reflection: keep every one,
# including the generated *_Impl classes and their constructors.
-keep class * extends androidx.room.RoomDatabase { *; }
-keep class **_Impl { <init>(); }
-keep class androidx.work.impl.WorkDatabase_Impl { *; }

# WorkManager and the start-up helpers that create it at launch.
-keep class androidx.work.** { *; }
-keep class androidx.startup.** { *; }
-keep class * extends androidx.startup.Initializer { *; }

# DataStore (used by shared_preferences / Firebase) reads protobuf fields by name.
-keep class * extends androidx.datastore.preferences.protobuf.GeneratedMessageLite { *; }
