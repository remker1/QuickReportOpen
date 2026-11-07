// =============================================================================
// Translations.h - public entry point of the main application's i18n table.
// Supported languages: English (en), French (fr), Chinese (zh).
// Lookup at runtime is done by MainWindow::tr()-style helpers using these maps.
// =============================================================================
#ifndef TRANSLATIONS_H
#define TRANSLATIONS_H

#include <QMap>
#include <QString>

// Populates `translations` (language code -> key -> text) with every QuickReports
// UI string. The source of truth is a single key -> {en, fr, zh} table in
// Translations.cpp; add new strings there. Tester-module strings are added
// separately via addTesterTranslations().
void loadQuickReportsTranslations(QMap<QString, QMap<QString, QString>> &translations);

#endif // TRANSLATIONS_H
