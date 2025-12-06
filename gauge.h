#ifndef GAUGE_H
#define GAUGE_H
#include <QWidget>

class Gauge : public QWidget {
//    Q_OBJECT
public:
    Gauge(QWidget *parent);
    void setColors(const QColor low, const QColor high, int index = 0);
    void setInterval(uint ms = 1000);
    void setLabel(const QString label);
    void setPosition(Qt::Alignment a, int offsetX = 0, int offsetY = 0);
    void setRange(int min = 0, int max = 100, int index = 0);
    void setSize(int size);
    void setSource(const QString source, int index = 0);
    void setToolTip(const QString tip, uint cacheMs = 1000);
protected:
    void enterEvent(QEnterEvent *event) override;
    bool eventFilter(QObject *o, QEvent *e) override;
    void hideEvent(QHideEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
//    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
private:
    enum Type { Normal, Clock };
    void adjustGeometry();
    void readFromProcess();
    void readTipFromProcess();
    void updateValues();
    QColor m_colors[3][2];
    int m_range[3][2], m_value[3];
    uint m_interval, m_tipCache, m_labelFlags;
    QString m_source[3], m_tooltip, m_tooltipSource, m_label;
    qint64 m_lastTipDate;
    bool m_dirty;
    QTimer m_timer;
    Qt::Alignment m_align;
    QPoint m_offset;
    Type m_type;
};

#endif // GAUGE_H