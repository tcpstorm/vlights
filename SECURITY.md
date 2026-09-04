# Security

VLights is a client-side plugin that patches colour bytes in the game's own
memory and, once at startup, asks GitHub whether a newer release exists. It
runs no scripts, opens no ports, sends nothing but that one request, and
installs nothing.

## Reporting

If you find something that could let the plugin be used against the person
running it, please report it privately rather than in a public issue: use
GitHub's private vulnerability reporting on this repository (Security tab,
"Report a vulnerability"). You will get a reply within a week.

Crashes and wrong colours are not security issues; use the issue templates.

## Scope

Things that would count:

- the update check being made to fetch from, or trust, anything other than
  GitHub's release endpoint for this repository;
- the ini or log handling reading or writing outside the plugin's folder;
- a way for a server to make the plugin do something other than recolour
  lights.

Things that are out of scope:

- FiveM's anti-cheat flagging the plugin. Whether a plugin is allowed is the
  server's decision; the plugin does nothing to hide from it.
- Crashes caused by other plugins or mods editing the same game objects.

## Releases

Only builds attached to a release on this repository, produced by the
workflow in `.github/workflows/release.yml` from a tagged commit on `main`,
are official. Check a download against the file's Properties, Details tab:
product name VLights, the version of the release, author Storm.
