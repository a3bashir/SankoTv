#include "ConsistencyBoard.h"

#include "StoryboardModel.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QEnterEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QUuid>
#include <QVBoxLayout>
#include <Qt>

// ===== EntryCard ==========================================================

EntryCard::EntryCard(int index, QWidget *parent)
    : QFrame(parent)
    , m_index(index)
{
    setCursor(Qt::PointingHandCursor);
}

void EntryCard::enterEvent(QEnterEvent *)
{
    if (m_delete)
        m_delete->show();
}

void EntryCard::leaveEvent(QEvent *)
{
    if (m_delete)
        m_delete->hide();
}

void EntryCard::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
        emit clicked(m_index);
}

// ===== Helpers ============================================================

namespace {

const char *kImageFilter = "Images (*.png *.jpg *.jpeg *.bmp)";

QString joinTags(const QStringList &tags)
{
    return tags.join(QStringLiteral(", "));
}

QStringList splitTags(const QString &text)
{
    QStringList out;
    const QStringList parts = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &p : parts) {
        const QString t = p.trimmed();
        if (!t.isEmpty())
            out << t;
    }
    return out;
}

} // namespace

QPixmap ConsistencyBoard::letterbox(const QPixmap &source, int w, int h)
{
    QPixmap result(w, h);
    result.fill(Qt::black);
    if (!source.isNull()) {
        QPainter painter(&result);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        QSize target = source.size();
        target.scale(w, h, Qt::KeepAspectRatio);
        QRect r(QPoint(0, 0), target);
        r.moveCenter(result.rect().center());
        painter.drawPixmap(r, source);
    }
    return result;
}

QPixmap ConsistencyBoard::placeholder(const QString &name, int w, int h)
{
    QPixmap pm(w, h);
    pm.fill(QColor("#2a2a2a"));

    QString initials;
    const QStringList parts = name.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (int i = 0; i < parts.size() && initials.size() < 2; ++i)
        initials += parts.at(i).left(1).toUpper();
    if (initials.isEmpty())
        initials = QStringLiteral("?");

    QPainter painter(&pm);
    painter.setPen(QColor("#666666"));
    QFont font = painter.font();
    font.setPixelSize(qMax(12, h / 3));
    font.setBold(true);
    painter.setFont(font);
    painter.drawText(pm.rect(), Qt::AlignCenter, initials);
    return pm;
}

QWidget *ConsistencyBoard::typeBadge(const QString &type)
{
    QLabel *badge = new QLabel(type);
    const bool character = (type == QLatin1String("Character"));
    badge->setStyleSheet(
        QStringLiteral("background-color: %1; color: %2; border-radius: 4px;"
                       " padding: 2px 8px; font-size: 11px; font-weight: 600;")
            .arg(character ? QStringLiteral("#f5a623") : QStringLiteral("#4a90d9"),
                 character ? QStringLiteral("#0a0a0a") : QStringLiteral("#ffffff")));
    badge->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    return badge;
}

// ===== Construction =======================================================

ConsistencyBoard::ConsistencyBoard(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("background-color: #0a0a0a;"));

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    root->addWidget(createTopBar());

    QHBoxLayout *content = new QHBoxLayout;
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(0);
    content->addWidget(createLeftColumn());
    content->addWidget(createRightPanel(), 1);

    root->addLayout(content, 1);
}

QWidget *ConsistencyBoard::createTopBar()
{
    QWidget *bar = new QWidget;
    bar->setAttribute(Qt::WA_StyledBackground, true);
    bar->setFixedHeight(60);
    bar->setStyleSheet(QStringLiteral("background-color: #111111;"));

    QHBoxLayout *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(16, 0, 16, 0);
    layout->setSpacing(10);

    QPushButton *back = new QPushButton(QString::fromUtf8("\xE2\x86\x90"));
    back->setCursor(Qt::PointingHandCursor);
    back->setToolTip(QStringLiteral("Back to Storyboard"));
    back->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; color: #cccccc; border: none; font-size: 20px;"
        " padding: 4px 8px; } QPushButton:hover { color: #ffffff; }"));
    connect(back, &QPushButton::clicked, this, &ConsistencyBoard::backRequested);
    layout->addWidget(back);

    layout->addStretch(1);

    QLabel *title = new QLabel(QStringLiteral("Consistency Board"));
    title->setStyleSheet(QStringLiteral("color: #ffffff; font-size: 15px; font-weight: 600;"));
    layout->addWidget(title);

    layout->addStretch(1);

    QPushButton *addChar = new QPushButton(QStringLiteral("+ Add Character"));
    addChar->setCursor(Qt::PointingHandCursor);
    addChar->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #f5a623; color: #0a0a0a; border: none;"
        " border-radius: 6px; padding: 8px 14px; font-size: 13px; font-weight: 600; }"
        "QPushButton:hover { background-color: #ffb733; }"));
    connect(addChar, &QPushButton::clicked, this, [this] { addEntry(QStringLiteral("Character")); });
    layout->addWidget(addChar);

    QPushButton *addLoc = new QPushButton(QStringLiteral("+ Add Location"));
    addLoc->setCursor(Qt::PointingHandCursor);
    addLoc->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; color: #4a90d9; border: 1px solid #4a90d9;"
        " border-radius: 6px; padding: 8px 14px; font-size: 13px; font-weight: 600; }"
        "QPushButton:hover { background-color: rgba(74,144,217,40); }"));
    connect(addLoc, &QPushButton::clicked, this, [this] { addEntry(QStringLiteral("Location")); });
    layout->addWidget(addLoc);

    return bar;
}

QWidget *ConsistencyBoard::createLeftColumn()
{
    QWidget *column = new QWidget;
    column->setAttribute(Qt::WA_StyledBackground, true);
    column->setFixedWidth(260);
    column->setStyleSheet(QStringLiteral("background-color: #111111;"));

    QVBoxLayout *layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    QScrollArea *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; border: none; }"));

    QWidget *container = new QWidget;
    container->setStyleSheet(QStringLiteral("background: transparent;"));
    m_listLayout = new QVBoxLayout(container);
    m_listLayout->setContentsMargins(10, 12, 10, 12);
    m_listLayout->setSpacing(8);
    m_listLayout->addStretch(1);

    scroll->setWidget(container);
    layout->addWidget(scroll, 1);

    return column;
}

QWidget *ConsistencyBoard::createRightPanel()
{
    QWidget *panel = new QWidget;
    panel->setAttribute(Qt::WA_StyledBackground, true);
    panel->setStyleSheet(QStringLiteral("background-color: #0a0a0a;"));

    QVBoxLayout *outer = new QVBoxLayout(panel);
    outer->setContentsMargins(28, 24, 28, 24);
    outer->setSpacing(0);

    m_detailLayout = new QVBoxLayout;
    m_detailLayout->setContentsMargins(0, 0, 0, 0);
    m_detailLayout->setSpacing(14);
    outer->addLayout(m_detailLayout, 1);

    return panel;
}

// ===== Data plumbing ======================================================

void ConsistencyBoard::setEntries(QVector<ConsistencyEntry> *entries)
{
    m_entries = entries;
    refresh();
}

void ConsistencyBoard::refresh()
{
    m_selected = -1;
    m_editing = false;
    rebuildList();
    rebuildDetail();
}

// ===== Left list ==========================================================

void ConsistencyBoard::rebuildList()
{
    if (!m_listLayout)
        return;
    while (QLayoutItem *item = m_listLayout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }

    if (m_entries) {
        for (int i = 0; i < m_entries->size(); ++i) {
            const ConsistencyEntry &entry = m_entries->at(i);

            EntryCard *card = new EntryCard(i);
            card->setObjectName(QStringLiteral("entryCard"));
            const bool selected = (i == m_selected);
            card->setStyleSheet(
                selected
                    ? QStringLiteral("QFrame#entryCard { background-color: #1b1b1b;"
                                     " border: 1px solid #2a2a2a; border-left: 3px solid #f5a623;"
                                     " border-radius: 6px; }")
                    : QStringLiteral("QFrame#entryCard { background-color: #161616;"
                                     " border: 1px solid #2a2a2a; border-radius: 6px; }"));
            connect(card, &EntryCard::clicked, this, &ConsistencyBoard::selectEntry);

            QHBoxLayout *cardLayout = new QHBoxLayout(card);
            cardLayout->setContentsMargins(8, 8, 8, 8);
            cardLayout->setSpacing(8);

            QLabel *thumb = new QLabel;
            thumb->setFixedSize(80, 45);
            thumb->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            thumb->setStyleSheet(QStringLiteral("border: none;"));
            thumb->setPixmap(entry.thumbnail.isNull()
                                 ? placeholder(entry.name, 80, 45)
                                 : entry.thumbnail.scaled(80, 45, Qt::IgnoreAspectRatio,
                                                          Qt::SmoothTransformation));
            cardLayout->addWidget(thumb);

            QVBoxLayout *textLayout = new QVBoxLayout;
            textLayout->setContentsMargins(0, 0, 0, 0);
            textLayout->setSpacing(3);

            QLabel *name = new QLabel(entry.name);
            name->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            name->setStyleSheet(QStringLiteral(
                "color: #ffffff; font-size: 13px; font-weight: 600; border: none;"));
            textLayout->addWidget(name);

            QWidget *badge = typeBadge(entry.type);
            QHBoxLayout *badgeRow = new QHBoxLayout;
            badgeRow->setContentsMargins(0, 0, 0, 0);
            badgeRow->addWidget(badge);
            badgeRow->addStretch(1);
            textLayout->addLayout(badgeRow);

            QString desc = entry.description.simplified();
            if (desc.size() > 60)
                desc = desc.left(60) + QString::fromUtf8("\xE2\x80\xA6");
            QLabel *descLabel = new QLabel(desc);
            descLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            descLabel->setWordWrap(true);
            descLabel->setStyleSheet(QStringLiteral("color: #888888; font-size: 11px; border: none;"));
            textLayout->addWidget(descLabel);

            cardLayout->addLayout(textLayout, 1);

            // Hover-revealed delete button.
            QPushButton *del = new QPushButton(QString::fromUtf8("\xC3\x97"), card);
            del->setCursor(Qt::PointingHandCursor);
            del->setToolTip(QStringLiteral("Delete entry"));
            del->setFixedSize(20, 20);
            del->setStyleSheet(QStringLiteral(
                "QPushButton { background: transparent; color: #888888; border: none;"
                " font-size: 15px; } QPushButton:hover { color: #e06c6c; }"));
            del->hide();
            connect(del, &QPushButton::clicked, this, [this, i] { deleteEntry(i); });
            card->setDeleteButton(del);
            cardLayout->addWidget(del, 0, Qt::AlignTop);

            m_listLayout->addWidget(card);
        }
    }

    m_listLayout->addStretch(1);
}

// ===== Right detail =======================================================

void ConsistencyBoard::rebuildDetail()
{
    if (!m_detailLayout)
        return;
    while (QLayoutItem *item = m_detailLayout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        if (QLayout *l = item->layout()) {
            while (QLayoutItem *sub = l->takeAt(0)) {
                if (QWidget *sw = sub->widget())
                    sw->deleteLater();
                delete sub;
            }
        }
        delete item;
    }
    m_editName = nullptr;
    m_editDesc = nullptr;
    m_editTags = nullptr;
    m_editThumbLabel = nullptr;

    // Nothing selected -> placeholder.
    if (!m_entries || m_selected < 0 || m_selected >= m_entries->size()) {
        QLabel *empty = new QLabel(QStringLiteral("Select an entry or add a new one"));
        empty->setAlignment(Qt::AlignCenter);
        empty->setStyleSheet(QStringLiteral("color: #666666; font-size: 14px;"));
        m_detailLayout->addStretch(1);
        m_detailLayout->addWidget(empty);
        m_detailLayout->addStretch(1);
        return;
    }

    const ConsistencyEntry &entry = m_entries->at(m_selected);
    const QString labelStyle =
        QStringLiteral("color: #888888; font-size: 11px; font-weight: 600; letter-spacing: 1px;");

    if (!m_editing) {
        // ---- View mode --------------------------------------------------
        QLabel *thumb = new QLabel;
        thumb->setFixedSize(480, 270);
        thumb->setStyleSheet(QStringLiteral("border: 1px solid #2a2a2a;"));
        thumb->setPixmap(entry.thumbnail.isNull()
                             ? placeholder(entry.name, 480, 270)
                             : letterbox(entry.thumbnail, 480, 270));
        m_detailLayout->addWidget(thumb, 0, Qt::AlignLeft);

        QHBoxLayout *headRow = new QHBoxLayout;
        headRow->setContentsMargins(0, 0, 0, 0);
        headRow->setSpacing(12);

        QLabel *name = new QLabel(entry.name);
        name->setStyleSheet(QStringLiteral("color: #ffffff; font-size: 24px; font-weight: 700;"));
        headRow->addWidget(name);
        headRow->addWidget(typeBadge(entry.type));
        headRow->addStretch(1);

        QPushButton *edit = new QPushButton(QStringLiteral("Edit"));
        edit->setCursor(Qt::PointingHandCursor);
        edit->setStyleSheet(QStringLiteral(
            "QPushButton { background-color: #1c1c1c; color: #ffffff; border: 1px solid #2a2a2a;"
            " border-radius: 6px; padding: 7px 18px; font-size: 13px; }"
            "QPushButton:hover { background-color: #262626; }"));
        connect(edit, &QPushButton::clicked, this, &ConsistencyBoard::startEdit);
        headRow->addWidget(edit);
        m_detailLayout->addLayout(headRow);

        QLabel *notesLabel = new QLabel(QStringLiteral("REFERENCE NOTES"));
        notesLabel->setStyleSheet(labelStyle);
        m_detailLayout->addWidget(notesLabel);

        QLabel *notes = new QLabel(entry.description.isEmpty()
                                       ? QStringLiteral("(no notes)")
                                       : entry.description);
        notes->setWordWrap(true);
        notes->setStyleSheet(QStringLiteral("color: #dddddd; font-size: 13px;"));
        m_detailLayout->addWidget(notes);

        QLabel *tagsLabel = new QLabel(QStringLiteral("TAGS"));
        tagsLabel->setStyleSheet(labelStyle);
        m_detailLayout->addWidget(tagsLabel);

        QHBoxLayout *tagsRow = new QHBoxLayout;
        tagsRow->setContentsMargins(0, 0, 0, 0);
        tagsRow->setSpacing(6);
        if (entry.tags.isEmpty()) {
            QLabel *none = new QLabel(QStringLiteral("(no tags)"));
            none->setStyleSheet(QStringLiteral("color: #666666; font-size: 12px;"));
            tagsRow->addWidget(none);
        } else {
            for (const QString &tag : entry.tags) {
                QLabel *pill = new QLabel(tag);
                pill->setStyleSheet(QStringLiteral(
                    "background-color: rgba(245,166,35,40); color: #f5a623; border-radius: 9px;"
                    " padding: 2px 10px; font-size: 11px;"));
                tagsRow->addWidget(pill);
            }
        }
        tagsRow->addStretch(1);
        m_detailLayout->addLayout(tagsRow);

        m_detailLayout->addStretch(1);
        return;
    }

    // ---- Edit mode ------------------------------------------------------
    m_editThumbLabel = new QLabel;
    m_editThumbLabel->setFixedSize(480, 270);
    m_editThumbLabel->setStyleSheet(QStringLiteral("border: 1px solid #2a2a2a;"));
    m_editThumbLabel->setPixmap(m_editThumb.isNull()
                                    ? placeholder(entry.name, 480, 270)
                                    : letterbox(m_editThumb, 480, 270));
    m_detailLayout->addWidget(m_editThumbLabel, 0, Qt::AlignLeft);

    QPushButton *changeImg = new QPushButton(QStringLiteral("Change Image"));
    changeImg->setCursor(Qt::PointingHandCursor);
    changeImg->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #1c1c1c; color: #cccccc; border: 1px solid #2a2a2a;"
        " border-radius: 6px; padding: 6px 14px; font-size: 12px; }"
        "QPushButton:hover { background-color: #262626; }"));
    connect(changeImg, &QPushButton::clicked, this, &ConsistencyBoard::changeImage);
    m_detailLayout->addWidget(changeImg, 0, Qt::AlignLeft);

    const QString fieldStyle = QStringLiteral(
        "QLineEdit, QPlainTextEdit { background-color: #1a1a1a; color: #ffffff;"
        " border: 1px solid #2a2a2a; border-radius: 4px; padding: 6px; font-size: 13px; }");

    QLabel *nameLabel = new QLabel(QStringLiteral("NAME"));
    nameLabel->setStyleSheet(labelStyle);
    m_detailLayout->addWidget(nameLabel);
    m_editName = new QLineEdit(entry.name);
    m_editName->setStyleSheet(fieldStyle);
    m_detailLayout->addWidget(m_editName);

    QLabel *descLabel = new QLabel(QStringLiteral("REFERENCE NOTES"));
    descLabel->setStyleSheet(labelStyle);
    m_detailLayout->addWidget(descLabel);
    m_editDesc = new QPlainTextEdit(entry.description);
    m_editDesc->setStyleSheet(fieldStyle);
    m_editDesc->setFixedHeight(120);
    m_detailLayout->addWidget(m_editDesc);

    QLabel *tagsLabel = new QLabel(QStringLiteral("TAGS (comma-separated)"));
    tagsLabel->setStyleSheet(labelStyle);
    m_detailLayout->addWidget(tagsLabel);
    m_editTags = new QLineEdit(joinTags(entry.tags));
    m_editTags->setStyleSheet(fieldStyle);
    m_detailLayout->addWidget(m_editTags);

    QHBoxLayout *buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 8, 0, 0);
    buttons->addStretch(1);

    QPushButton *cancel = new QPushButton(QStringLiteral("Cancel"));
    cancel->setCursor(Qt::PointingHandCursor);
    cancel->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #1c1c1c; color: #cccccc; border: 1px solid #2a2a2a;"
        " border-radius: 6px; padding: 8px 18px; font-size: 13px; }"
        "QPushButton:hover { background-color: #262626; }"));
    connect(cancel, &QPushButton::clicked, this, &ConsistencyBoard::cancelEdit);
    buttons->addWidget(cancel);

    QPushButton *save = new QPushButton(QStringLiteral("Save"));
    save->setCursor(Qt::PointingHandCursor);
    save->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #f5a623; color: #0a0a0a; border: none;"
        " border-radius: 6px; padding: 8px 18px; font-size: 13px; font-weight: 600; }"
        "QPushButton:hover { background-color: #ffb733; }"));
    connect(save, &QPushButton::clicked, this, &ConsistencyBoard::saveEdit);
    buttons->addWidget(save);

    m_detailLayout->addLayout(buttons);
    m_detailLayout->addStretch(1);
}

// ===== Actions ============================================================

void ConsistencyBoard::selectEntry(int index)
{
    if (!m_entries || index < 0 || index >= m_entries->size())
        return;
    m_selected = index;
    m_editing = false;
    rebuildList();
    rebuildDetail();
}

void ConsistencyBoard::deleteEntry(int index)
{
    if (!m_entries || index < 0 || index >= m_entries->size())
        return;
    m_entries->removeAt(index);
    if (m_selected == index)
        m_selected = -1;
    else if (m_selected > index)
        --m_selected;
    m_editing = false;
    rebuildList();
    rebuildDetail();
}

void ConsistencyBoard::addEntry(const QString &type)
{
    if (!m_entries)
        return;

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Add %1").arg(type));
    dialog.setStyleSheet(QStringLiteral(
        "QDialog { background-color: #161616; }"
        "QLabel { color: #cccccc; }"
        "QLineEdit, QPlainTextEdit { background-color: #1a1a1a; color: #ffffff;"
        " border: 1px solid #2a2a2a; border-radius: 4px; padding: 5px; }"));

    QFormLayout *form = new QFormLayout(&dialog);

    QLineEdit *nameEdit = new QLineEdit;
    nameEdit->setPlaceholderText(QStringLiteral("Required"));
    form->addRow(QStringLiteral("Name"), nameEdit);

    QPlainTextEdit *descEdit = new QPlainTextEdit;
    descEdit->setFixedHeight(90);
    form->addRow(QStringLiteral("Description"), descEdit);

    QLineEdit *tagsEdit = new QLineEdit;
    tagsEdit->setPlaceholderText(QStringLiteral("comma, separated, tags"));
    form->addRow(QStringLiteral("Tags"), tagsEdit);

    QPixmap chosen; // captured by the upload lambda

    QLabel *preview = new QLabel;
    preview->setFixedSize(160, 90);
    preview->setStyleSheet(QStringLiteral("border: 1px solid #2a2a2a;"));
    preview->setPixmap(placeholder(QStringLiteral("?"), 160, 90));

    QPushButton *upload = new QPushButton(QStringLiteral("Upload Image..."));
    upload->setCursor(Qt::PointingHandCursor);
    connect(upload, &QPushButton::clicked, &dialog, [&] {
        const QString file = QFileDialog::getOpenFileName(
            &dialog, QStringLiteral("Choose reference image"), QString(),
            QString::fromLatin1(kImageFilter));
        if (file.isEmpty())
            return;
        QPixmap loaded(file);
        if (loaded.isNull())
            return;
        chosen = letterbox(loaded, 320, 180);
        preview->setPixmap(chosen.scaled(160, 90, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    });
    form->addRow(QStringLiteral("Image"), upload);
    form->addRow(QString(), preview);

    QDialogButtonBox *box =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(box);
    connect(box, &QDialogButtonBox::accepted, &dialog, [&] {
        if (nameEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dialog, QStringLiteral("Add %1").arg(type),
                                 QStringLiteral("A name is required."));
            return;
        }
        dialog.accept();
    });
    connect(box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted)
        return;

    ConsistencyEntry entry;
    entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    entry.name = nameEdit->text().trimmed();
    entry.type = type;
    entry.description = descEdit->toPlainText();
    entry.tags = splitTags(tagsEdit->text());
    entry.thumbnail = chosen; // null if none uploaded
    m_entries->append(entry);

    m_selected = m_entries->size() - 1;
    m_editing = false;
    rebuildList();
    rebuildDetail();
}

void ConsistencyBoard::startEdit()
{
    if (!m_entries || m_selected < 0 || m_selected >= m_entries->size())
        return;
    m_editing = true;
    m_editThumb = m_entries->at(m_selected).thumbnail;
    m_editThumbChanged = false;
    rebuildDetail();
}

void ConsistencyBoard::changeImage()
{
    const QString file = QFileDialog::getOpenFileName(
        this, QStringLiteral("Choose reference image"), QString(),
        QString::fromLatin1(kImageFilter));
    if (file.isEmpty())
        return;
    QPixmap loaded(file);
    if (loaded.isNull())
        return;
    m_editThumb = letterbox(loaded, 320, 180);
    m_editThumbChanged = true;
    if (m_editThumbLabel) // update in place so typed fields aren't lost
        m_editThumbLabel->setPixmap(letterbox(m_editThumb, 480, 270));
}

void ConsistencyBoard::saveEdit()
{
    if (!m_entries || m_selected < 0 || m_selected >= m_entries->size())
        return;
    ConsistencyEntry &entry = (*m_entries)[m_selected];
    if (m_editName) {
        const QString name = m_editName->text().trimmed();
        if (!name.isEmpty())
            entry.name = name;
    }
    if (m_editDesc)
        entry.description = m_editDesc->toPlainText();
    if (m_editTags)
        entry.tags = splitTags(m_editTags->text());
    if (m_editThumbChanged)
        entry.thumbnail = m_editThumb;

    m_editing = false;
    rebuildList();
    rebuildDetail();
}

void ConsistencyBoard::cancelEdit()
{
    m_editing = false;
    rebuildDetail();
}
