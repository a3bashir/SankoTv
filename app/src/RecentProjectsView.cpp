#include "RecentProjectsView.h"
#include "RecentThumbnails.h"
#include "SankoTheme.h"
#include "brushlib/StudioControls.h"

#include <QApplication>
#include <QFileInfo>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>

namespace studio = brushlib::studio;

namespace {

// The card's own surface colours. These are the placeholder cards' values,
// carried over from the stylesheet they were written in when the cards
// became painted - not new colours.
const QColor kCardBg(0x16, 0x16, 0x16);
const QColor kCardBorder(0x2a, 0x2a, 0x2a);
const QColor kThumbWell(0x33, 0x33, 0x33);
const QColor kCardTitle(0xff, 0xff, 0xff);

QFont interFont(int pixelSize, QFont::Weight weight)
{
    QFont f(QStringLiteral("Inter"));
    f.setPixelSize(pixelSize);
    f.setWeight(weight);
    return f;
}
// Cards keep the type the placeholder cards had (the application font at
// 15 / 12 px); rows keep the type they had in the dialog (Inter 11 / 10).
// Each came from somewhere, and neither was restyled on the way.
QFont appFont(int pixelSize, QFont::Weight weight)
{
    QFont f = QApplication::font();
    f.setPixelSize(pixelSize);
    f.setWeight(weight);
    return f;
}
QFont cardTitleFont() { return appFont(15, QFont::DemiBold); }
QFont cardDateFont() { return appFont(12, QFont::Normal); }
QFont rowTitleFont() { return interFont(11, QFont::Medium); }
QFont rowDateFont() { return interFont(10, QFont::Normal); }

// The film-strip glyph for a project with no picture (design 350:109) -
// painted, no asset: frame outline + sprocket holes. Drawn in a 16 px box
// and scaled, so the card's larger glyph is the row's glyph, not a redraw.
void paintFilmIcon(QPainter &p, const QPointF &centre, qreal size,
                   const QColor &color)
{
    p.save();
    p.translate(centre);
    p.scale(size / 16.0, size / 16.0);
    p.translate(-8.0, -8.0);
    const QRectF r(0, 0, 16, 16);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(color, 1.2));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r.adjusted(0.6, 0.6, -0.6, -0.6), 2, 2);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    for (int i = 0; i < 3; ++i) {
        const qreal y = r.top() + 3.2 + i * (r.height() - 6.4) / 2.0;
        p.drawRect(QRectF(r.left() + 2.2, y - 0.9, 1.8, 1.8));
        p.drawRect(QRectF(r.right() - 4.0, y - 0.9, 1.8, 1.8));
    }
    p.restore();
}

QString dateText(const QDateTime &lastOpened)
{
    return QStringLiteral("Last opened: ")
        + lastOpened.toString(QStringLiteral("MMM d, yyyy"));
}

} // namespace

RecentProjectsView::RecentProjectsView(RecentThumbnails *thumbnails,
                                       QWidget *parent)
    : QWidget(parent), m_thumbnails(thumbnails)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setFixedSize(kViewW, 1);
    connect(m_thumbnails, &RecentThumbnails::ready, this,
            [this](const QString &) { update(); });
}

void RecentProjectsView::reload()
{
    m_items.clear();
    const QVector<RecentProjects::Entry> entries = RecentProjects::entries();
    for (const RecentProjects::Entry &entry : entries) {
        Item item;
        item.entry = entry;
        item.missing = !QFileInfo::exists(entry.path);
        m_items.append(item);
    }
    for (int i = 0; i < m_items.size(); ++i)
        if (!m_items.at(i).missing)
            m_thumbnails->request(m_items.at(i).entry.path,
                                  thumbPixelSize(kindAt(i)));

    m_hover = m_pressed = -1;
    // The keyboard cursor does not survive a re-read: the list is re-ordered
    // whenever a project is opened, so an index kept from before would be
    // sitting on a different project than the one it was left on.
    m_current = hasFocus() && !m_items.isEmpty() ? 0 : -1;

    const int rows = qMax(0, int(m_items.size()) - kCardSlots);
    const int rowLines = (rows + kCardSlots - 1) / kCardSlots;
    int height = m_items.isEmpty() ? 1 : kCardH;
    if (rowLines > 0)
        height += kSectionGap + rowLines * kRowPitch - (kRowPitch - kRowH);
    setFixedSize(kViewW, height);
    update();
    emit reloaded();
}

RecentProjectsView::Kind RecentProjectsView::kindAt(int index) const
{
    return index < kCardSlots ? Kind::Card : Kind::Row;
}

// Three FIXED slots: one or two recents fill from the left and the unused
// slots draw nothing, so the first card is always in the same place.
QRect RecentProjectsView::rectAt(int index) const
{
    if (index < 0 || index >= m_items.size())
        return QRect();
    if (index < kCardSlots)
        return QRect(index * (kCardW + kCardGap), 0, kCardW, kCardH);
    const int n = index - kCardSlots;
    return QRect((n % kCardSlots) * (kCardW + kCardGap),
                 kCardH + kSectionGap + (n / kCardSlots) * kRowPitch, kCardW,
                 kRowH);
}

QRect RecentProjectsView::thumbRectAt(int index) const
{
    const QRect r = rectAt(index);
    if (r.isNull())
        return QRect();
    if (kindAt(index) == Kind::Card)
        return QRect(r.left() + kCardPad, r.top() + kCardPad, kThumbW, kThumbH);
    return QRect(r.left() + 6, r.top() + 6, kRowThumbW, kRowThumbH);
}

QRect RecentProjectsView::nameRectAt(int index) const
{
    const QRect r = rectAt(index);
    if (kindAt(index) == Kind::Card)
        return QRect(r.left() + kCardPad, r.top() + kCardPad + kThumbH + 12,
                     kThumbW, 20);
    return QRect(r.left() + 64, r.top() + 8, r.width() - 64 - 6, 14);
}

QString RecentProjectsView::pathAt(int index) const
{
    return index >= 0 && index < m_items.size() ? m_items.at(index).entry.path
                                                : QString();
}

// MIDDLE-elided so version-suffixed families keep their distinguishing
// tail (Cyberpunk_Alley_v1 / v2 / v3).
QString RecentProjectsView::shownNameAt(int index) const
{
    if (index < 0 || index >= m_items.size())
        return QString();
    const QFont font =
        kindAt(index) == Kind::Card ? cardTitleFont() : rowTitleFont();
    return QFontMetrics(font).elidedText(
        QFileInfo(m_items.at(index).entry.path).completeBaseName(),
        Qt::ElideMiddle, nameRectAt(index).width());
}

QString RecentProjectsView::shownDateAt(int index) const
{
    return index >= 0 && index < m_items.size()
        ? dateText(m_items.at(index).entry.lastOpened)
        : QString();
}

bool RecentProjectsView::missingAt(int index) const
{
    return index >= 0 && index < m_items.size() && m_items.at(index).missing;
}

QSize RecentProjectsView::thumbPixelSize(Kind kind) const
{
    const qreal dpr = devicePixelRatioF();
    // The row well is inset by its 1 px border, as it was in the dialog.
    const QSize logical = kind == Kind::Card
        ? QSize(kThumbW, kThumbH)
        : QSize(kRowThumbW - 2, kRowThumbH - 2);
    return QSize(qRound(logical.width() * dpr), qRound(logical.height() * dpr));
}

bool RecentProjectsView::hasThumbnailAt(int index) const
{
    if (index < 0 || index >= m_items.size() || m_items.at(index).missing)
        return false;
    return !m_thumbnails
                ->pixmap(m_items.at(index).entry.path,
                         thumbPixelSize(kindAt(index)))
                .isNull();
}

// The film glyph is drawn for a project KNOWN to have no picture: its file
// is gone, or its thumbnail has been looked for and there is none. Not for
// one whose picture simply has not been decoded yet.
bool RecentProjectsView::showsGlyphAt(int index) const
{
    if (index < 0 || index >= m_items.size())
        return false;
    const Item &item = m_items.at(index);
    if (item.missing)
        return true;
    const QSize size = thumbPixelSize(kindAt(index));
    return m_thumbnails->answered(item.entry.path, size)
        && m_thumbnails->pixmap(item.entry.path, size).isNull();
}

void RecentProjectsView::setRemovePrompt(
    std::function<bool(const QString &)> prompt)
{
    m_removePrompt = std::move(prompt);
}

void RecentProjectsView::activate(int index)
{
    if (index < 0 || index >= m_items.size())
        return;
    const QString path = m_items.at(index).entry.path;
    // Checked NOW, not from the flag set at load: the file may have come
    // back, or gone, while the page sat open.
    if (!QFileInfo::exists(path)) {
        // Never a silent failure: offer to drop the dead entry.
        bool remove = false;
        if (m_removePrompt) {
            remove = m_removePrompt(path);
        } else {
            remove = QMessageBox::question(
                         window(), QStringLiteral("Project Not Found"),
                         QStringLiteral("The project file was not found:\n%1"
                                        "\n\nRemove it from Recent Projects?")
                             .arg(path),
                         QMessageBox::Yes | QMessageBox::No)
                == QMessageBox::Yes;
        }
        if (remove)
            RecentProjects::remove(path);
        reload(); // either way: the dim state follows what is on disk now
        return;
    }
    emit openRequested(path);
}

int RecentProjectsView::indexAt(const QPoint &pos) const
{
    for (int i = 0; i < m_items.size(); ++i)
        if (rectAt(i).contains(pos))
            return i;
    return -1;
}

// The thumbnail well. The cached pixmap COVERS the well (it was decoded
// that way), so the centred crop here is a copy of pixels, never a resample
// on a paint.
void RecentProjectsView::paintThumb(QPainter &p, int index, const QRectF &well,
                                    qreal radius, qreal glyph) const
{
    const Item &item = m_items.at(index);
    const QPixmap px = item.missing
        ? QPixmap()
        : m_thumbnails->pixmap(item.entry.path, thumbPixelSize(kindAt(index)));
    if (px.isNull()) {
        // The glyph MEANS "no picture". While a picture is still queued the
        // well is left empty instead, or every project would flash the
        // glyph on the way to its thumbnail each time the page opens.
        if (showsGlyphAt(index))
            paintFilmIcon(p, well.center(), glyph, studio::kFieldLabel);
        return;
    }
    const qreal dpr = devicePixelRatioF();
    const QSizeF want(well.width() * dpr, well.height() * dpr);
    const QRectF source((px.width() - want.width()) / 2.0,
                        (px.height() - want.height()) / 2.0, want.width(),
                        want.height());
    p.save();
    QPainterPath clip;
    clip.addRoundedRect(well, radius, radius);
    p.setClipPath(clip);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawPixmap(well, px, source);
    p.restore();
}

void RecentProjectsView::paintCard(QPainter &p, int index) const
{
    const Item &item = m_items.at(index);
    const QRectF r = QRectF(rectAt(index)).adjusted(0.5, 0.5, -0.5, -0.5);
    const bool cursor = index == m_current && hasFocus();
    p.setPen(QPen(cursor ? SankoTheme::kAccentLight
                         : (index == m_hover ? studio::kFieldBorderHover
                                             : kCardBorder),
                  1.0));
    p.setBrush(kCardBg);
    p.drawRoundedRect(r, 10, 10);

    // A missing project stays listed but visibly dimmed at 50%.
    p.setOpacity(item.missing ? 0.5 : 1.0);
    const QRectF well(thumbRectAt(index));
    p.setPen(Qt::NoPen);
    p.setBrush(kThumbWell);
    p.drawRoundedRect(well, 6, 6);
    paintThumb(p, index, well, 6, 32);

    p.setFont(cardTitleFont());
    p.setPen(kCardTitle);
    p.drawText(nameRectAt(index), Qt::AlignVCenter | Qt::AlignLeft,
               shownNameAt(index));
    p.setFont(cardDateFont());
    p.setPen(studio::kFieldLabel);
    const QRect name = nameRectAt(index);
    p.drawText(QRect(name.left(), name.bottom() + 1 + 6, name.width(), 16),
               Qt::AlignVCenter | Qt::AlignLeft, shownDateAt(index));
    p.setOpacity(1.0);
}

void RecentProjectsView::paintRow(QPainter &p, int index) const
{
    const Item &item = m_items.at(index);
    const QRect row = rectAt(index);
    if (index == m_current && hasFocus()) {
        p.setPen(QPen(SankoTheme::kAccentLight, 1.0));
        p.setBrush(studio::kFieldBg);
        p.drawRoundedRect(QRectF(row).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    } else if (index == m_hover) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 10));
        p.drawRoundedRect(row, 4, 4);
    }
    p.setOpacity(item.missing ? 0.5 : 1.0);
    const QRectF thumb(thumbRectAt(index));
    p.setPen(QPen(studio::kFieldBorder, 1.0));
    p.setBrush(studio::kFieldBg);
    p.drawRoundedRect(thumb.adjusted(0.5, 0.5, -0.5, -0.5), 2, 2);
    paintThumb(p, index, thumb.adjusted(1, 1, -1, -1), 2, 16);

    p.setFont(rowTitleFont());
    p.setPen(studio::kFieldText);
    p.drawText(nameRectAt(index), Qt::AlignVCenter | Qt::AlignLeft,
               shownNameAt(index));
    p.setFont(rowDateFont());
    p.setPen(studio::kFieldLabel);
    p.drawText(QRect(row.left() + 64, row.top() + 23, row.width() - 70, 13),
               Qt::AlignVCenter | Qt::AlignLeft, shownDateAt(index));
    p.setOpacity(1.0);
}

// Thumbnails are decoded for one device pixel ratio. Dragged to a monitor
// with another, the page would show the film glyph for every project until
// it was next opened - so ask again at the new size.
bool RecentProjectsView::event(QEvent *event)
{
    if (event->type() == QEvent::DevicePixelRatioChange)
        reload();
    return QWidget::event(event);
}

void RecentProjectsView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    for (int i = 0; i < m_items.size(); ++i) {
        if (kindAt(i) == Kind::Card)
            paintCard(p, i);
        else
            paintRow(p, i);
    }
}

void RecentProjectsView::mouseMoveEvent(QMouseEvent *event)
{
    const int i = indexAt(event->position().toPoint());
    if (i != m_hover) {
        m_hover = i;
        setCursor(i >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void RecentProjectsView::leaveEvent(QEvent *)
{
    m_hover = -1;
    update();
}

void RecentProjectsView::mousePressEvent(QMouseEvent *event)
{
    m_pressed = event->button() == Qt::LeftButton
        ? indexAt(event->position().toPoint())
        : -1;
}

// A click is a press and a release on the SAME item: pressing a card and
// sliding off it before letting go opens nothing.
void RecentProjectsView::mouseReleaseEvent(QMouseEvent *event)
{
    const int pressed = m_pressed;
    m_pressed = -1;
    if (event->button() != Qt::LeftButton || pressed < 0)
        return;
    if (indexAt(event->position().toPoint()) == pressed) {
        m_current = pressed;
        activate(pressed);
    }
}

void RecentProjectsView::keyPressEvent(QKeyEvent *event)
{
    if (m_items.isEmpty()) {
        QWidget::keyPressEvent(event);
        return;
    }
    const int last = int(m_items.size()) - 1;
    switch (event->key()) {
    case Qt::Key_Right:
        m_current = qMin(last, m_current + 1);
        break;
    case Qt::Key_Left:
        m_current = qMax(0, m_current - 1);
        break;
    case Qt::Key_Down:
        m_current = qMin(last, m_current + kCardSlots);
        break;
    case Qt::Key_Up:
        m_current = qMax(0, m_current - kCardSlots);
        break;
    case Qt::Key_Home:
        m_current = 0;
        break;
    case Qt::Key_End:
        m_current = last;
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        activate(m_current);
        return;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    update();
}

void RecentProjectsView::focusInEvent(QFocusEvent *event)
{
    if (m_current < 0 && !m_items.isEmpty())
        m_current = 0;
    update();
    QWidget::focusInEvent(event);
}

void RecentProjectsView::focusOutEvent(QFocusEvent *event)
{
    update();
    QWidget::focusOutEvent(event);
}
