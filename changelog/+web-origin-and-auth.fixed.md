The web interface no longer accepts a change coming from another site, the status page and the splash image require the configured login like every other page, and Wi-Fi credentials are checked before they are stored:

- A page served from a different host can no longer post to the receiver. Browsers attach the stored credentials to a cross-site form post, so the login alone did not prevent it; a request whose Origin/Referer names another host is now refused with 403. Requests without either header (curl, scripts) still work.
- `GET /` and `GET /splash.png` were reachable without a login and exposed the receiver's IP, MAC address, SSID and firmware version.
- An SSID longer than 32 bytes, or a password that is neither empty nor 8-63 bytes, is rejected instead of being stored truncated: the truncated value made the receiver fail to associate with a password that still looked correct on the page.
- A splash upload is abandoned as soon as an error response has been queued, and no longer dereferences a missing upload context.
