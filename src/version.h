// Single source of truth for the app's runtime version string.
//
// Release builds get this stamped in by build.bat via /D (MSVC) or -D
// (clang), driven by the FTW_VERSION env var the release workflow sets
// from the pushed git tag (.github/workflows/release.yml strips the
// leading 'v', so tag v1.2.3 -> "1.2.3"). Local/dev builds fall back to
// "0.0.0-dev" below since nothing defines FTW_VERSION for them.
//
// This is also the value a future in-app update checker should compare
// against the GitHub Releases API's `tag_name` (releases/latest).
#ifndef VERSION_H
#define VERSION_H

#ifndef FTW_VERSION
#define FTW_VERSION "0.0.0-dev"
#endif

#endif // VERSION_H
