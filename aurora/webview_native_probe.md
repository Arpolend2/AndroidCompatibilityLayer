# Native Aurora WebView diagnostic

This independent diagnostic is retained for SDK investigation. It is not the
Android WebView backend. The rejected Android-to-QML/Aurora provider, transport,
host, Canvas capture consumer, input adapter and their mock tests were removed.
Android apps continue to use Android's `WebViewFactory` and Chromium loader.

With Aurora SDK 5.2.1, configure the project with
`ANDROID_COMPATIBILITY_LAYER_BUILD_TEST_TARGETS=ON` and
`ANDROID_COMPATIBILITY_LAYER_BUILD_WEBVIEW_PROBE=ON`, then build and run
`android_compatibility_layer_webview_native_probe` in a graphical Aurora
environment as a non-root user. This target is not installed with the manager.
It loads fixed offline HTML with JavaScript disabled and checks the title.
An exit error, timeout or crash is a failure. The SDK container's minimal Qt
platform crashes during Chromium initialization and is not a runtime test.

Passing `--frame` requests a 120x80 red page and checks a copied software frame.
An accelerated-only renderer or timeout fails this particular test. This mode
has not been verified on a device.

The separate `aurora-webview-probe` project beside this repository is unchanged.
Its device results apply only to its own native Aurora WebView: offline HTML
loaded, JavaScript did not change the title, the software-frame test timed out
with an accelerated renderer, and subsequent Qt item-grab checks captured the
fixed colors. Its synthetic native text replacement tests observed caret
mismatches. None verifies Android rendering, input, IME, interception, cookie
handling, JavaScript interfaces, sandboxing or microG authentication.
Its opt-in Google page diagnostic is not microG's activity; do not enter
credentials. Displaying that page does not establish login or token delivery.

For the selected Android provider, runtime paths, missing-provider diagnostics
and remaining device prerequisites, see the Android System WebView section in
`aurora/README.md`. Do not re-enable a QML bridge or disable Chromium security to
work around an absent provider.
