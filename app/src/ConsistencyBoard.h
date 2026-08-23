#pragma once

#include <QFrame>
#include <QVector>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QVBoxLayout;

struct ConsistencyEntry;

// A clickable entry card with a hover-revealed delete button. Child widgets are
// transparent for mouse events so hover/click land on the card itself.
class EntryCard : public QFrame
{
    Q_OBJECT

public:
    explicit EntryCard(int index, QWidget *parent = nullptr);
    void setDeleteButton(QPushButton *button) { m_delete = button; }

signals:
    void clicked(int index);

protected:
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    int m_index;
    QPushButton *m_delete = nullptr;
};

// Character & location reference library. Reads/writes a MainWindow-owned
// QVector<ConsistencyEntry> via a non-owning pointer.
class ConsistencyBoard : public QWidget
{
    Q_OBJECT

public:
    explicit ConsistencyBoard(QWidget *parent = nullptr);

    void setEntries(QVector<ConsistencyEntry> *entries); // non-owning
    void refresh();                                      // rebuild from entries

signals:
    void backRequested();
    // This board edits the OWNER's entry vector in place (it holds a pointer
    // to it), and those entries are serialized with the project — but none
    // of it is undoable, so the window's undo-stack watcher cannot see it.
    // Adding, deleting or editing an entry says so here.
    void documentChanged();

public:
    // Test hook: the REAL deletion path (adding one opens a modal dialog,
    // so deletion is the mutation a gate can drive end to end).
    void deleteEntryForTest(int index) { deleteEntry(index); }

private:
    QWidget *createTopBar();
    QWidget *createLeftColumn();
    QWidget *createRightPanel();

    void rebuildList();
    void rebuildDetail();

    void selectEntry(int index);
    void addEntry(const QString &type);
    void deleteEntry(int index);

    void startEdit();
    void saveEdit();
    void cancelEdit();
    void changeImage();

    static QPixmap letterbox(const QPixmap &source, int w, int h);
    static QPixmap placeholder(const QString &name, int w, int h);
    static QWidget *typeBadge(const QString &type);

    QVector<ConsistencyEntry> *m_entries = nullptr;
    int m_selected = -1;
    bool m_editing = false;

    QVBoxLayout *m_listLayout = nullptr;
    QVBoxLayout *m_detailLayout = nullptr;

    // Edit-mode widgets (valid only while editing).
    QLineEdit *m_editName = nullptr;
    QPlainTextEdit *m_editDesc = nullptr;
    QLineEdit *m_editTags = nullptr;
    QLabel *m_editThumbLabel = nullptr;
    QPixmap m_editThumb;
    bool m_editThumbChanged = false;
};
