// =============================================================================
// TesterTranslations.h - i18n strings of the hardware tester (keys start with ht_).
// =============================================================================
#ifndef TESTERTRANSLATIONS_H
#define TESTERTRANSLATIONS_H

#include <QMap>
#include <QString>

// Merges every hardware-tester string (keys prefixed "ht_") into the app's
// translation tables. Called at the end of MainWindow::loadTranslations() so
// the tester keys live here instead of doubling that already-huge function.
void addTesterTranslations(QMap<QString, QMap<QString, QString>> &translations);

#endif // TESTERTRANSLATIONS_H
