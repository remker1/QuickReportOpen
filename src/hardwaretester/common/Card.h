// =============================================================================
// Card.h - rounded, titled container widget used by all tester pages.
// =============================================================================
#ifndef TESTERCARD_H
#define TESTERCARD_H

#include <QFrame>

class QLabel;
class QVBoxLayout;

// Rounded, titled card used across all tester pages. Styling comes from the
// app QSS via objectName "TesterCard"; the title via "TesterCardTitle".
class Card : public QFrame {
    Q_OBJECT
public:
    explicit Card(const QString &title = QString(), QWidget *parent = nullptr);

    void setTitle(const QString &title);
    // Layout that content should be added into (below the title).
    QVBoxLayout *body() const { return m_body; }

private:
    // title label (hidden when empty)
    QLabel *m_title;
    QVBoxLayout *m_body;
};

#endif // TESTERCARD_H
