#ifndef VERSION_H
#define VERSION_H

// Application version — single source of truth.
//
// Format: MAJOR.MINOR.BUILD, no leading-zero padding (e.g. 1.0.0, 1.12.130).
//   MAJOR (left)   — changed only on explicit request.
//   MINOR (middle) — +1 for each functional change (a feature added or removed).
//   BUILD (right)  — +1 for each build & deploy.
//
// Update this string when the rules above call for it; main.cpp and the
// sidebar version label both read it.
#define APP_VERSION "1.41.71"

#endif // VERSION_H
