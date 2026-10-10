/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
extern "C" {
#include <libavcodec/version.h>
}

// The headers we compile against must be the major version we link, or every
// FFmpeg struct silently gets the wrong layout. For example, the offset of
// AVCodecParameters::coded_side_data is 176 in libavcodec 60 and 32 in 62, so
// the app reads pointers from the wrong fields and crashes inside libavcodec
// with no hint of where the mismatch came from. purple/build_app.sh pins
// ffmpeg@6; nothing else detects this because the functions tdesktop calls
// exist in both versions, so a mismatched build still links cleanly.
static_assert(LIBAVCODEC_VERSION_MAJOR == 60,
	"Building against the wrong FFmpeg headers. See purple/build_app.sh - a "
	"machine with the full \"ffmpeg\" formula installed has its headers in "
	"/opt/homebrew/include, which the compiler reaches before ffmpeg@6's own.");
