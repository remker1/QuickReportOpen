// =============================================================================
// Card.cpp - implementation of the Card container.
// =============================================================================
#include "Card.h"

#include <QLabel>
#include <QVBoxLayout>

// Builds the frame: optional title label on top, body layout below.
Card::Card(const QString &title, QWidget *parent) : QFrame(parent) {
    setObjectName("TesterCard");
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 12, 16, 14);
    outer->setSpacing(8);

    m_title = new QLabel(title, this);
    m_title->setObjectName("TesterCardTitle");
    m_title->setVisible(!title.isEmpty());
    outer->addWidget(m_title);

    m_body = new QVBoxLayout();
    m_body->setContentsMargins(0, 0, 0, 0);
    m_body->setSpacing(6);
    outer->addLayout(m_body);
}

// Changes the title; an empty title hides the label.
void Card::setTitle(const QString &title) {
    m_title->setText(title);
    m_title->setVisible(!title.isEmpty());
}
