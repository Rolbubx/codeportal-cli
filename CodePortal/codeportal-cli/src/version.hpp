// version.hpp : numéro de version de CodePortal, à un seul endroit.
// Lu par le code, par res/codeportal.rc (propriétés de l'exe) et par le workflow de publication
// (.github/workflows/release.yml), qui vérifie que le tag vX.Y.Z correspond.
#pragma once

#define CODEPORTAL_VERSION_MAJOR 1
#define CODEPORTAL_VERSION_MINOR 3
#define CODEPORTAL_VERSION_PATCH 1
#define CODEPORTAL_VERSION "1.3.1"

#ifndef RC_INVOKED
constexpr const char* kVersion = CODEPORTAL_VERSION;
#endif
