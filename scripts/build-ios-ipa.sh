#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-ios-xcode}"
PROJECT="$BUILD_DIR/ARMSX2iOS.xcodeproj"
IPA_NAME="${IPA_NAME:-ARMSX2-iOS-unsigned.ipa}"
APP_PATH="$BUILD_DIR/Release-iphoneos/ARMSX2iOS.app"
STAGING_DIR="$BUILD_DIR/ipa-staging"
BUILD_LOG="$BUILD_DIR/xcodebuild.log"

if [[ ! -d "$PROJECT" ]]; then
	"$ROOT_DIR/scripts/generate-ios-xcode.sh"
fi

mkdir -p "$BUILD_DIR"

if ! command -v xcodebuild >/dev/null 2>&1; then
	echo "error: xcodebuild was not found. Install full Xcode from Apple, then run:" >&2
	echo "  sudo xcode-select -s /Applications/Xcode.app/Contents/Developer" >&2
	exit 1
fi

if ! xcrun --sdk iphoneos --find metal >/dev/null 2>&1 || ! xcrun --sdk iphoneos --find metallib >/dev/null 2>&1; then
	echo "error: the iPhoneOS Metal compiler tools were not found." >&2
	echo "Make sure full Xcode is selected, not Command Line Tools only:" >&2
	echo "  sudo xcode-select -s /Applications/Xcode.app/Contents/Developer" >&2
	echo "  xcodebuild -downloadPlatform iOS" >&2
	exit 1
fi

echo "Using Xcode:"
xcodebuild -version
echo "Developer directory: $(xcode-select -p)"
echo "Metal compiler: $(xcrun --sdk iphoneos --find metal)"
echo

set +e
xcodebuild \
	-project "$PROJECT" \
	-scheme ARMSX2iOS \
	-configuration Release \
	-sdk iphoneos \
	CODE_SIGNING_ALLOWED=NO \
	CODE_SIGNING_REQUIRED=NO \
	CODE_SIGN_IDENTITY="" \
	build 2>&1 | tee "$BUILD_LOG"
XCODEBUILD_STATUS=${PIPESTATUS[0]}
set -e

if [[ "$XCODEBUILD_STATUS" -ne 0 ]]; then
	if grep -q "CompileMetalFile" "$BUILD_LOG"; then
		echo >&2
		echo "Metal shader compilation failed. Common fixes:" >&2
		echo "  1. Select full Xcode: sudo xcode-select -s /Applications/Xcode.app/Contents/Developer" >&2
		echo "  2. Install the iOS platform in Xcode Settings > Platforms, or run: xcodebuild -downloadPlatform iOS" >&2
		echo "  3. Open $BUILD_LOG and search above the CompileMetalFile line for the first shader error." >&2
	fi
	exit "$XCODEBUILD_STATUS"
fi

if [[ ! -d "$APP_PATH" ]]; then
	echo "error: built app was not found at $APP_PATH" >&2
	exit 1
fi

rm -rf "$STAGING_DIR"
mkdir -p "$STAGING_DIR/Payload"
ditto "$APP_PATH" "$STAGING_DIR/Payload/ARMSX2iOS.app"
(cd "$STAGING_DIR" && zip -qry "$BUILD_DIR/$IPA_NAME" Payload)

echo "Created unsigned IPA:"
echo "  $BUILD_DIR/$IPA_NAME"
