#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-ios-xcode}"
PROJECT="$BUILD_DIR/ARMSX2iOS.xcodeproj"
IPA_NAME="${IPA_NAME:-ARMSX2-iOS-unsigned.ipa}"
APP_PATH="$BUILD_DIR/Release-iphoneos/ARMSX2iOS.app"
STAGING_DIR="$BUILD_DIR/ipa-staging"

if [[ ! -d "$PROJECT" ]]; then
	"$ROOT_DIR/scripts/generate-ios-xcode.sh"
fi

xcodebuild \
	-project "$PROJECT" \
	-scheme ARMSX2iOS \
	-configuration Release \
	-sdk iphoneos \
	CODE_SIGNING_ALLOWED=NO \
	CODE_SIGNING_REQUIRED=NO \
	CODE_SIGN_IDENTITY="" \
	build

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
