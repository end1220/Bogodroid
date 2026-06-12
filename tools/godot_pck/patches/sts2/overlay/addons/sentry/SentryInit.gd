extends Node
# StS2 patch: original autoload init script referenced SentrySDK/SentryEvent
# classes from the sentry GDExtension, which has no linux.arm64 prebuilt.
# Replaced with a no-op Node so any leftover res:// reference still resolves.
