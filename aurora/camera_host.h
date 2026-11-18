#pragma once

#include "../core/include/megler.h"

// Process-lifetime host. Construct after QGuiApplication, before megler_run.
// Call auroraCameraShutdown before destroying QGuiApplication.
megler_camera_host auroraCameraHost();

// Stops cameras and joins the worker. Idempotent; late host calls are safe.
// Do not call from a camera callback or the camera worker thread.
void auroraCameraShutdown();
