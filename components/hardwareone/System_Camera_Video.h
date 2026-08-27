/**
 * System_Camera_Video - MJPEG-in-AVI video recording for ESP32-S3 camera
 *
 * Writes an OpenDML/AVI 1.0 file with Motion JPEG video stream. Each JPEG
 * frame from captureFrame() is appended as a 00dc chunk; an idx1 table is
 * written at the end so players (VLC, QuickTime) can seek.
 *
 * Recording targets SD card only — MJPEG bitrate at even QVGA/10fps exceeds
 * sustained LittleFS write throughput, so onboard flash would drop frames.
 * The UI is expected to gate the Record button on VFS::isSDAvailable().
 */

#ifndef SYSTEM_CAMERA_VIDEO_H
#define SYSTEM_CAMERA_VIDEO_H

#include <Arduino.h>
#include "System_BuildConfig.h"

// ── Recorder API (camera builds only) ────────────────────────────────────────
#if ENABLE_CAMERA_SENSOR

// Runtime recording state — mirrors the micRecording pattern. Read by the
// sensor status JSON and the web UI; set only by startVideoRecording /
// stopVideoRecording. Start/stop are serialized (mutex) so G2 stream and
// web CLI cannot double-open an AVI.
extern bool videoRecording;

// Start a new recording. Creates /sd/videos/ if missing, opens an AVI file,
// writes the header skeleton, and spawns the recorder task.
// Returns false if: camera disabled, SD unavailable, already recording, or
// file/task creation fails.
bool startVideoRecording();

// Stop the active recording. Signals the task to exit, waits for it to
// finalize the AVI header/index, and closes the file. Safe to call when not
// recording (no-op).
void stopVideoRecording();

// Path + frame count of the most recently finalized recording (valid until the
// next startVideoRecording()). Used for the camerarecord-stop confirmation.
const char* videoLastRecordingPath();
uint32_t    videoLastRecordingFrames();

#endif  // ENABLE_CAMERA_SENSOR

// ── Viewer API (base experience — every build) ───────────────────────────────
// Listing/serving recordings needs only an SD card, not a camera, so these are
// available on all boards. They return empty/false when SD is unavailable or
// no recordings exist.

// Enumerate recordings as a colon-delimited "filename:size" list joined by
// newlines, matching the mic recordings format.
String getVideoRecordingsList();

// Delete a specific recording by filename (no path). Returns true on success.
bool deleteVideoRecording(const String& filename);

#endif  // SYSTEM_CAMERA_VIDEO_H
