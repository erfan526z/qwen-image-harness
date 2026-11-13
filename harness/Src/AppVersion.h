#pragma once

// APP_VERSION is injected by qmake (QImageHarness.pro) from the APP_VERSION
// environment variable so CI can stamp every artifact with the release version.
// The fallback keeps plain editor builds working without qmake.
#ifndef APP_VERSION
#define APP_VERSION "0.1.0"
#endif
