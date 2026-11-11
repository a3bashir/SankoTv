#pragma once

// LAYERS AT REST (batch 6b-V, 2026-11-07; Q12 V, the measurement of 2026-11-06).
//
// A layer's pixels, as the 256 px tiles that hold something and one value for
// everything else: nothing for empty space, and a plain Background is only its
// colour. The tile grid is anchored to the FRAME's top-left, so a stage margin is
// only a rectangle: changing one moves no tile, and art outside the stage stays
// here, hidden, until the stage grows back or Trim Hidden Art frees it (Q13).
//
// Immutable once made, and shared: a layer, its copies in the undo history and a
// worker read the same one. Every function here is safe on any thread.
//
// This is the document's storage at rest, not the paint engine's TiledImage (a
// mirror of the one panel on the canvas, with its mipmaps and dirty sets). The
// fold and the unfold are row copies, measured against it on the user's projects
// (app/build/_probe/stage_measure/results_parallel.txt).

#include <QImage>
#include <QPoint>
#include <QRect>
#include <QVector>

#include <memory>

class QPainter;
class QPainterPath;

namespace layertiles {

inline constexpr int kTile = 256;

struct Tile
{
    int x = 0, y = 0;  // its index from the frame's top-left; negative in the margin
    QImage pixels;     // kTile x kTile, ARGB32_Premultiplied
};

struct Tiles
{
    QVector<Tile> tiles;   // by y, then x
    quint32 fill = 0;      // the premultiplied pixel everything else reads as
    qint64 key = 0;        // the identity of these pixels: see Layer::contentKey
    QRect drawn;           // the union of the tiles' rects, frame space; empty: all fill
    qint64 bytes() const { return qint64(tiles.size()) * kTile * kTile * 4; }
};
using TilesPtr = std::shared_ptr<const Tiles>;

// Only this format is folded; a layer in any other stays a full image.
bool foldable(const QImage &image);
// What a Background reads as outside its tiles: its colour when it is plain, the
// colour it had before when it is painted, else white paper.
quint32 paperFill(const QImage &image, const TilesPtr &previous);
// `image`'s pixels as tiles. `frameAt`: where the frame's top-left is in `image`.
// `previous`: the layer's tiles so far, whose art outside `image` is kept.
TilesPtr fold(const QImage &image, const QPoint &frameAt, quint32 fill, const TilesPtr &previous = {},
              qint64 key = 0);
// Nothing but `fill`, with `key` as its identity.
TilesPtr empty(quint32 fill, qint64 key);
// The pixels of `rect` (frame space) as a full image; the same bytes they were.
QImage unfold(const Tiles &tiles, const QRect &rect);
// Paints the part inside `stage` (frame space), the stage's top-left at the
// painter's origin, as drawImage of the unfolded image paints. The painter's
// transform must be the identity.
void paint(QPainter &painter, const Tiles &tiles, const QRect &stage);
// The tight bounds of every pixel with any alpha inside `stage`, in stage px.
QRect drawnBounds(const Tiles &tiles, const QRect &stage);
// Any pixel other than the fill outside `stage`: art kept and hidden.
bool hasHidden(const Tiles &tiles, const QRect &stage);
// The tight bounds of every pixel other than the fill OUTSIDE `stage`, in frame
// space (batch B1: the art the Move tool lifts with the rest); empty: none.
QRect hiddenBounds(const Tiles &tiles, const QRect &stage);
// The same tiles without what lies outside `stage`, a new identity.
TilesPtr trimmed(const Tiles &tiles, const QRect &stage);
// The identities: unique, and never one of QImage::cacheKey().
qint64 freshKey();

// WHERE THE ART IS (batch 6c, Stage mode): a map of cells of kArtCell px on the
// frame's grid, on when one holds a pixel other than the fill. Fine enough to
// outline art on screen, cheap enough to ask of a whole layer once.
inline constexpr int kArtCell = 8;
struct ArtCells
{
    QRect cells;       // in cells, frame space; empty: nothing drawn
    QVector<uchar> on; // cells.width() x cells.height(), by row
    bool isOn(int x, int y) const
    {
        return cells.contains(x, y) && on.at((y - cells.top()) * cells.width() + x - cells.left());
    }
    bool anyIn(const QRect &rect) const; // frame space
};
ArtCells artCells(const Tiles &tiles);
// The edge of all of them together, frame space: closed, stair-stepped by the cell.
QPainterPath artOutline(const QVector<ArtCells> &layers);

} // namespace layertiles
