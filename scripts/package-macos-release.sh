#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-macos-ui}"
APP_NAME="${APP_NAME:-ARMSX2-MacOS 2.1}"
APP_DIR="${APP_DIR:-$BUILD_DIR/$APP_NAME.app}"
ZIP_NAME="${ZIP_NAME:-$APP_NAME.zip}"
ZIP_PATH="${ZIP_PATH:-$BUILD_DIR/$ZIP_NAME}"
SIGN_IDENTITY="${SIGN_IDENTITY:--}"
NOTARY_PROFILE="${NOTARY_PROFILE:-}"
BUILD_APP="${BUILD_APP:-0}"
VERIFY_GATEKEEPER="${VERIFY_GATEKEEPER:-0}"

if [[ "$BUILD_APP" == "1" ]]; then
	SIGN_IDENTITY="$SIGN_IDENTITY" "$ROOT_DIR/scripts/build-macos-ui.sh"
fi

if [[ ! -d "$APP_DIR" ]]; then
	echo "error: app bundle not found: $APP_DIR" >&2
	exit 1
fi

codesign --verify --deep --strict --verbose=2 "$APP_DIR"

if [[ -n "$NOTARY_PROFILE" ]]; then
	if [[ "$SIGN_IDENTITY" == "-" ]]; then
		echo "error: notarization requires a Developer ID Application signing identity." >&2
		echo "Set SIGN_IDENTITY='Developer ID Application: ...' and NOTARY_PROFILE='<keychain profile>'." >&2
		exit 1
	fi

	upload_zip="$(mktemp -t ARMSX2-notary.XXXXXX).zip"
	rm -f "$upload_zip"
	ditto -c -k --keepParent --sequesterRsrc --zlibCompressionLevel 9 "$APP_DIR" "$upload_zip"

	xcrun notarytool submit "$upload_zip" --keychain-profile "$NOTARY_PROFILE" --wait
	rm -f "$upload_zip"

	xcrun stapler staple "$APP_DIR"
	xcrun stapler validate "$APP_DIR"
	VERIFY_GATEKEEPER=1
fi

if [[ "$VERIFY_GATEKEEPER" == "1" ]]; then
	spctl -a -vvv -t exec "$APP_DIR"
elif ! spctl -a -vvv -t exec "$APP_DIR"; then
	echo
	echo "warning: Gatekeeper did not accept this app. Public downloads need Developer ID signing and notarization." >&2
	echo "         Example: SIGN_IDENTITY='Developer ID Application: Name (TEAMID)' NOTARY_PROFILE=armsx2 scripts/build-macos-release.sh" >&2
fi

rm -f "$ZIP_PATH"
ditto -c -k --keepParent --sequesterRsrc --zlibCompressionLevel 9 "$APP_DIR" "$ZIP_PATH"

echo "Created macOS release zip:"
echo "  $ZIP_PATH"
