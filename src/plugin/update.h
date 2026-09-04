// update.h - "a newer release exists" notice for the menu. No auto-update:
// one HTTPS GET of the repository's latest release from GitHub's public API
// a few seconds after startup, on its own thread, compared against the
// running version. Off with update_check = 0. Never contacts anything but
// api.github.com, sends nothing but the request, and a failure is silent.
#pragma once

#include <string>

namespace vlights::update
{
	// Worker thread: starts the check once (no-op on later calls).
	void Start();

	// True when a release newer than the running version was seen; `latest`
	// receives its version (without the leading "v") and `url` the page.
	bool Available(std::string& latest, std::string& url);

	// Human-readable outcome for the log / menu: "checking", "up to date",
	// "update available", or why it could not check.
	const char* Status();
}
