#ifndef GAUGE_H
#define GAUGE_H
#include <QTimer>
#include <QWidget>

class Gauge : public QWidget {
    Q_OBJECT
public:
    enum ThreshType { None = 0, Maximum, Minimum };
    Gauge(QWidget *parent);
    void setColors(const QColor low, const QColor high, int index = 0);
    void setCriticalThreshold(int value, ThreshType type, const QString msg = QString(), int index = 0);
    void setInterval(uint ms = 1000);
    void setLabel(const QString label);
    void setMouseAction(QString action, Qt::MouseButton btn = Qt::LeftButton);
    void setPosition(Qt::Alignment a, int offsetX = 0, int offsetY = 0);
    void setRange(int min = 0, int max = 100, int index = 0);
    void setSize(int size);
    void setSource(QString source, int index = 0);
    void setToolTip(const QString tip, uint cacheMs = 1000);
    void setThresholdsRedundant(bool redundant);
    void setWheelAction(QString action, Qt::ArrowType direction);
signals:
    void critical(const QString &message, int ring);
    void uncritical(int ring);
protected:
    void enterEvent(QEnterEvent *event) override;
    bool eventFilter(QObject *o, QEvent *e) override;
    void leaveEvent(QEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
private:
    enum Type { Normal, Clock, Memory };
    void adjustGeometry();
    void checkCritical(int i);
    void readFromProcess();
    void readTipFromProcess();
    void updateValues();
    QColor m_colors[3][2];
    int m_range[3][2], m_value[3], m_threshValue[3];
    uint m_interval, m_tipCache, m_labelFlags;
    QString m_source[3], m_tooltip, m_tooltipSource, m_label, m_threshWarning[3];
    QHash<Qt::MouseButton, QString> m_mouseActions;
    QString m_wheelAction[4];
    qint64 m_lastTipDate;
    bool m_dirty;
    QTimer m_timer;
    Qt::Alignment m_align;
    QPoint m_offset;
    Type m_type;
    ThreshType m_threshType[3];
    bool m_wasCritical[3], m_criticalGroup;
};

#endif // GAUGE_H