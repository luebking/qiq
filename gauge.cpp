#include <QDateTime>
#include <QEnterEvent>
#include <QFile>
#include <QHash>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <QToolTip>
#include "gauge.h"

enum LabelFlags { P1 = 0, P2, P3, V1, V2, V3, DV1, DV2, DV3, CV1, CV2, CV3, MV1, MV2, MV3 };

Gauge::Gauge(QWidget *parent) : QWidget(parent) {
    for (int i = 0; i < 3; ++i) {
        m_range[i][0] = 0; m_range[i][1] = 100;
        m_threshType[i] = None;
        m_wasCritical[i] = false;
    }
    m_interval = 1000;
    m_tipCache = 1000;
    m_dirty = false;
    resize(128,128);
    m_type = Normal;
    connect(&m_timer, &QTimer::timeout, this, &Gauge::updateValues);
    if (parent)
        parent->installEventFilter(this);
}

void Gauge::setSource(QString source, int i) {
    if (source == "%clock%") {
        m_type = Clock;
        setRange(0, 0, 0);
    } else if (source.startsWith("%mem%")) {
        m_type = Memory;
        source.remove(0, 5);
    } else if (!source.isEmpty()) {
        m_type = Normal;
    }
    m_source[i] = source;
    if (m_source[i].isEmpty()) {
        m_value[i] = 0;
//        m_timer.stop();
    } else {
        m_timer.start(m_interval);
        updateValues();
    }
}

void Gauge::readFromProcess() {
    QProcess *p = qobject_cast<QProcess*>(sender());
    if (!p) {
        qDebug() << "wtf got us here?" << sender();
        return;
    }
    int i = p->property("gauge_index").toInt();
    if (i < 0 || i > 2) {
        qDebug() << "invalid index" << i;
        return;
    }
    const QStringList lines = QString::fromUtf8(p->readAllStandardOutput()).split('\n');
    bool ok;
    for (const QString &line : lines) {
        m_value[i] = line.toInt(&ok, 0);
        if (ok)
            break;
    }
    if (!ok) {
        qDebug() << "Could not read number from" << m_source[i];
        m_value[i] = 0;
    }
    p->deleteLater();
    if (ok)
        checkCritical(i);
    if (isVisible())
        update();
}

void Gauge::checkCritical(int i) {
    auto emitWarning = [=](const QString fallback) {
        if (m_threshWarning[i].isEmpty()) {
            emit critical(fallback, m_criticalGroup ? 0 : i);
            return;
        }
        QString msg = m_threshWarning[i];
        int percent = qRound(100*(m_value[i]-m_range[i][0])/double(m_range[i][1]-m_range[i][0]));
        msg.replace(QString("%p"), QString::number(percent));
        msg.replace(QString("%v"), QString::number(m_value[i]));
        msg.replace(QString("%dv"), QString::number(m_value[i]/10));
        msg.replace(QString("%cv"), QString::number(m_value[i]/100));
        msg.replace(QString("%mv"), QString::number(m_value[i]/1000));
        emit critical(msg, m_criticalGroup ? 0 : i);
    };
    auto groupOk = [=]() {
        if (!m_criticalGroup)
            return false;
        for (int j = 0; j < 3; ++j) {
            if (j == i || m_threshType[j] == None)
                continue;
            if (!m_wasCritical[j])
                return true;
        }
        return false;
    };
    if (m_threshType[i] == Maximum && m_value[i] > m_threshValue[i]) {
        m_wasCritical[i] = true;
        if (!groupOk())
            emitWarning(QString("%1 > %2").arg(m_value[i]).arg(m_threshValue[i]));
    } else if (m_threshType[i] == Minimum && m_value[i] < m_threshValue[i]) {
        if (!groupOk())
            emitWarning(QString("%1 < %2").arg(m_value[i]).arg(m_threshValue[i]));
        m_wasCritical[i] = true;
    } else if (m_wasCritical[i]) {
        m_wasCritical[i] = false;
        emit uncritical(m_criticalGroup ? 0 : i);
    }
}

void Gauge::updateValues() {
    if (!(isVisible() || m_threshValue[0] || m_threshValue[1] || m_threshValue[2])) {
        m_dirty = true;
        return;
    }
    m_dirty = false;
    if (m_type == Clock) {
        QTime time = QTime::currentTime();
        m_value[0] = time.second();
        m_value[1] = time.minute();
        m_value[2] = time.hour();
        update();
        return;
    }
    if (m_type == Memory) {
        QFile f("/proc/meminfo");
        if (f.exists() && f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            bool ok;
            QString s;
            QStringList pair;
            QHash<QString,qulonglong> meminfo;
            static QRegularExpression seps("[:[:space:]]+");
            const QStringList lines = QString::fromLocal8Bit(f.readAll()).split('\n', Qt::SkipEmptyParts);
            for (const QString &line : lines) {
                pair = line.split(seps);
                if (pair.size() < 2) {
                    qDebug() << "unexpected meminfo!!!" << pair;
                    continue;
                }
                const uint v = pair.at(1).toULongLong(&ok, 0); // VmallocTotal is gonna be huge…
                if (!ok) {
                    qDebug() << "unexpected meminfo, value is not a number!!!" << pair;
                    continue;
                }
                meminfo.insert(pair.at(0), v);
            }
            for (int i = 0; i < 3; ++i) {
                if (m_source[i].isEmpty())
                    continue;
                if (m_source[i] == "Zswapped") {
                    m_range[i][0] = 0;
                    m_range[i][1] = meminfo.value("Zswap");
                    m_value[i] = meminfo.value("Zswapped");
                } else if (m_source[i].startsWith("Swap")) {
                    m_range[i][0] = 0;
                    m_range[i][1] = meminfo.value("SwapTotal");
                    m_value[i] = meminfo.value(m_source[i]);
                } else {
                    m_range[i][0] = 0;
                    m_range[i][1] = meminfo.value("MemTotal");
                    m_value[i] = meminfo.value(m_source[i]);
                }
                checkCritical(i);
            }
            if (isVisible())
                update();
            return;
        }
        qDebug() << "unexpected meminfo, could not read /proc/meminfo!!!";
        return;
    }
    QMetaObject::Connection processDoneHandler[3];
    for (int i = 0; i < 3; ++i) {
        if (m_source[i].isEmpty())
            continue;
        QFile f(m_source[i]);
        if (f.exists()) {
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                bool ok;
                QString s;
                while (!f.atEnd()) {
                    s = QString::fromLocal8Bit(f.readLine());
                    m_value[i] = s.toInt(&ok, 0);
                    if (ok)
                        break;
                }
                if (!ok) {
                    qDebug() << "Could not read number from" << m_source[i];
                    m_value[i] = 0;
                    return;
                }
            } else {
                qDebug() << "Could not open" << m_source[i];
                return;
            }
            checkCritical(i);
            if (isVisible())
                update();
        } else {
            QProcess *p = new QProcess(this);
            p->setProperty("gauge_index", i);
            processDoneHandler[i] = connect (p, &QProcess::finished, this, &Gauge::readFromProcess);
            connect(p, &QProcess::finished, p, &QObject::deleteLater);
            QTimer::singleShot(m_interval, this, [=](){
                if (processDoneHandler[i]) {
                    qDebug() << m_source[i] << "takes longer than interval" << m_interval << " => killing!!";
                    disconnect(processDoneHandler[i]);
                    p->kill();
                }
            });
            p->startCommand(m_source[i]);
            if (!p->waitForStarted()) {
                p->deleteLater();
            }
        }
    }
}

void Gauge::setInterval(uint ms) {
    if (ms)
        m_timer.start(ms);
    else
        m_timer.stop();
}

void Gauge::setLabel(const QString label) {
    m_label = label;
    m_labelFlags = 0;
    for (int i = 0; i < 3; ++i) {
        if (m_label.contains(QString("%p%1").arg(i+1)))
            m_labelFlags |= 1<<i;
        if (m_label.contains(QString("%v%1").arg(i+1)))
            m_labelFlags |= 1<<(2+i);
        if (m_label.contains(QString("%dv%1").arg(i+1)))
            m_labelFlags |= 1<<(5+i);
        if (m_label.contains(QString("%cv%1").arg(i+1)))
            m_labelFlags |= 1<<(8+i);
        if (m_label.contains(QString("%mv%1").arg(i+1)))
            m_labelFlags |= 1<<(11+i);
    }
    update();
}

void Gauge::adjustGeometry() {
    QWidget *pw = parentWidget();
    if (!pw)
        return;
    QRect r = rect(), pwr = pw->rect();
    r.moveCenter(pwr.center());
    if (m_align & Qt::AlignLeft)
        r.moveLeft(0);
    if (m_align & Qt::AlignRight)
        r.moveRight(pwr.right());
    if (m_align & Qt::AlignTop)
        r.moveTop(0);
    if (m_align & Qt::AlignBottom)
        r.moveBottom(pwr.bottom());
    r.translate(m_offset);
    setGeometry(r);
}

bool Gauge::eventFilter(QObject *o, QEvent *e) {
    if (o == parentWidget() && e->type() == QEvent::Resize)
        adjustGeometry();
    return false;
}

void Gauge::setPosition(Qt::Alignment a, int offsetX, int offsetY) {
    m_align = a;
    m_offset = QPoint(offsetX, offsetY);
    adjustGeometry();
}

void Gauge::setSize(int size) {
    resize(size, size);
    adjustGeometry();
}

void Gauge::setRange(int min, int max, int i) {
    if (m_type == Clock) {
        m_range[0][0] = m_range[1][0] = m_range[2][0] = 0;
        m_range[0][1] = m_range[1][1] = 60;
        m_range[2][1] = 24;
        return;
    }
    m_range[i][0] = min; m_range[i][1] = max;
    updateValues();
}

void Gauge::setColors(const QColor low, const QColor high, int i) {
    m_colors[i][0] = low; m_colors[i][1] = high;
    update();
}

void Gauge::setCriticalThreshold(int value, ThreshType type, const QString msg, int i) {
    if (m_type == Clock)
        return; // no
    m_threshType[i] = type;
    m_threshValue[i] = value;
    m_threshWarning[i] = msg;
}

void Gauge::setThresholdsRedundant(bool redundant) {
    m_criticalGroup = redundant;
}

void Gauge::setToolTip(const QString tip, uint cacheMs) {
    m_tooltipSource = tip;
    m_tipCache = cacheMs;
    m_lastTipDate = 0;
}

void Gauge::readTipFromProcess() {
    QProcess *p = qobject_cast<QProcess*>(sender());
    if (!p) {
        qDebug() << "wtf got us here?" << sender();
        return;
    }
    m_tooltip = QString::fromLocal8Bit(p->readAllStandardOutput());
    m_lastTipDate = QDateTime::currentMSecsSinceEpoch();
    p->deleteLater();
    if (underMouse())
        QToolTip::showText(QCursor::pos(), m_tooltip, this, {}, 60000);
}

void Gauge::setMouseAction(QString action, Qt::MouseButton btn) {
    if (action.isEmpty())
        m_mouseActions.remove(btn);
    else
        m_mouseActions.insert(btn, action);
}

void Gauge::setWheelAction(QString action, Qt::ArrowType dir) {
    if (dir < 1 || dir > 4)
        return;
    m_wheelAction[dir-1] = action;
}

void Gauge::enterEvent(QEnterEvent *event) {
    QWidget::enterEvent(event);
    if (QDateTime::currentMSecsSinceEpoch() - m_lastTipDate < m_tipCache) {
        QToolTip::showText(event->globalPosition().toPoint(), m_tooltip, this, {}, 60000);
    } else {
        if (m_tooltipSource.isEmpty())
            return;
        QFile f(m_tooltipSource);
        if (f.exists()) {
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                m_tooltip = QString::fromLocal8Bit(f.readAll());
            } else {
                qDebug() << "Could not open" << m_tooltipSource;
                return;
            }
            m_lastTipDate = QDateTime::currentMSecsSinceEpoch();
            QToolTip::showText(event->globalPosition().toPoint(), m_tooltip, this, {}, 60000);
        } else {
            QProcess *p = new QProcess(this);
            QMetaObject::Connection processDoneHandler = connect (p, &QProcess::finished, this, &Gauge::readTipFromProcess);
            QTimer::singleShot(1000, this, [=](){
                if (processDoneHandler) {
                    qDebug() << m_tooltipSource << "takes longer than 1000 ms => killing!!";
                    disconnect(processDoneHandler);
                    p->kill();
                }
            });
            p->startCommand(m_tooltipSource);
            if (!p->waitForStarted()) {
                p->deleteLater();
                m_tooltip = m_tooltipSource;
                m_lastTipDate = QDateTime::currentMSecsSinceEpoch();
                QToolTip::showText(event->globalPosition().toPoint(), m_tooltip, this, {}, 60000);
            }
        }
    }
}
void Gauge::leaveEvent(QEvent *event) {
    QWidget::leaveEvent(event);
    QToolTip::hideText();
}

void Gauge::paintEvent(QPaintEvent *) {
    double percent[3] = {0,0,0};
    int s = qMin(width(), height());
    QPen pen;
    pen.setCapStyle(Qt::RoundCap);
    float f = 14 - 2*m_source[0].isEmpty() - 2*m_source[1].isEmpty() - 2*m_source[2].isEmpty();
    pen.setWidth(qMax(1, qRound(s/f)));

    s -= pen.width();
    QRect r(0,0,s,s);
    r.moveCenter(rect().center());

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(Qt::NoBrush);
    for (int i = 0; i < 3; ++i) {
        if (m_source[i].isEmpty())
            continue;

        percent[i] = (m_value[i]-m_range[i][0])/double(m_range[i][1]-m_range[i][0]);
        QColor c = m_colors[i][0];
        if (m_colors[i][0] != m_colors[i][1]) {
            int h,h2,s2,l,l2;
            m_colors[i][0].getHsl(&h,&s,&l);
            m_colors[i][1].getHsl(&h2,&s2,&l2);
            h = (1.0-percent[i])*h + percent[i]*h2;
            s = (1.0-percent[i])*s + percent[i]*s2;
            l = (1.0-percent[i])*l + percent[i]*l2;
            c = QColor::fromHsl(h, s, l);
        }
        c.setAlpha(64);
        pen.setColor(c);
        p.setPen(pen);
        p.drawEllipse(r);
        c.setAlpha(255);
        pen.setColor(c);
        p.setPen(pen);
        if (percent[i] > 0.99) // avoid butt-on-butt stuff
            p.drawEllipse(r);
        else
            p.drawArc(r, 90<<4, -5760.0*percent[i]);
        const int pw = pen.width()*1.2;
        r.adjust(pw,pw,-pw,-pw);
    }

    QFont fnt = font();
    QString label = m_label;
    if (m_type == Clock) {
        label = QDateTime::currentDateTime().toString(label);
    } else {
        for (int i = 0; i < 3; ++i) {
            if (m_labelFlags & 1<<i) {
                label.replace(QString("%p%1").arg(i+1), QString::number(qRound(percent[i]*100)));
            }
            if (m_labelFlags & 1<<(2+i))
                label.replace(QString("%v%1").arg(i+1), QString::number(m_value[i]));
            if (m_labelFlags & 1<<(5+i))
                label.replace(QString("%dv%1").arg(i+1), QString::number(m_value[i]/10));
            if (m_labelFlags & 1<<(8+i))
                label.replace(QString("%cv%1").arg(i+1), QString::number(m_value[i]/100));
            if (m_labelFlags & 1<<(11+i))
                label.replace(QString("%mv%1").arg(i+1), QString::number(m_value[i]/1000));
        }
    }
    QSize ts = QFontMetrics(fnt).size(0, label);
    qreal factor = qMin(r.width() / qreal(ts.width()), r.height() / qreal(ts.height()));
    if (factor != 1.0)
        fnt.setPointSize(fnt.pointSize()*factor);
    p.setFont(fnt);
    p.drawText(r, Qt::AlignCenter, label);
}

void Gauge::showEvent(QShowEvent *event) {
    if (m_dirty)
        updateValues();
    QWidget::showEvent(event);
}

void Gauge::mouseDoubleClickEvent(QMouseEvent *event) {
    return;
    QString command = m_mouseActions.value(event->button());
    if (command.isEmpty())
        return;
    QStringList args = QProcess::splitCommand(command);
    if (!args.isEmpty())
        command = args.takeFirst();
    QProcess::startDetached(command, args);
}


void Gauge::mouseReleaseEvent(QMouseEvent *event) {
    QString command = m_mouseActions.value(event->button());
    if (command.isEmpty())
        return;
    QStringList args = QProcess::splitCommand(command);
    if (!args.isEmpty())
        command = args.takeFirst();
    QProcess::startDetached(command, args);
}

void Gauge::wheelEvent(QWheelEvent *event) {
    Qt::ArrowType dir = Qt::NoArrow;
    QPoint d = event->angleDelta();
    if (event->inverted())
        d = -d;
    if (d.y() > 0)
        dir = Qt::UpArrow;
    else if (d.y() < 0)
        dir = Qt::DownArrow;
    else if (d.x() > 0)
        dir = Qt::LeftArrow;
    else if (d.y() < 0)
        dir = Qt::RightArrow;
    if (dir == Qt::NoArrow)
        return;
    QString command = m_wheelAction[dir-1];
    if (command.isEmpty())
        return;
    QStringList args = QProcess::splitCommand(command);
    if (!args.isEmpty())
        command = args.takeFirst();
    QProcess::startDetached(command, args);
//    updateValues();
}