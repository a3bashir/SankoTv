#pragma once

#include <QPlainTextEdit>

class QPaintEvent;
class QResizeEvent;

// A QPlainTextEdit with a line-number gutter on the left edge.
// Based on the canonical Qt "Code Editor" example, dark-themed.
class CodeEditor : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit CodeEditor(QWidget *parent = nullptr);

    void lineNumberAreaPaintEvent(QPaintEvent *event);
    int lineNumberAreaWidth();

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void updateLineNumberAreaWidth(int newBlockCount);
    void updateLineNumberArea(const QRect &rect, int dy);

private:
    QWidget *m_lineNumberArea;
};
