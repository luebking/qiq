/*
 *   Qiq shell for Qt6
 *   Copyright 2025 by Thomas Lübking <thomas.luebking@gmail.com>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License version 2
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details
 *
 *   You should have received a copy of the GNU General Public
 *   License along with this program; if not, write to the
 *   Free Software Foundation, Inc.,
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <QBoxLayout>
#include <QDateTime>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QFile>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QScreen>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QWindow>
#include "notifications.h"

Notification::Notification(QWidget *parent, uint id) : QFrame(parent), m_id(id), m_resident(false), m_countdown(nullptr) {
    QVBoxLayout *vl = new QVBoxLayout(this);
    QHBoxLayout *hl = new QHBoxLayout;
    hl->addWidget(m_icon = new QLabel(this));
    hl->addWidget(m_summary = new QLabel(this));
    m_summary->setProperty("role", "summary");
//    m_summary->setOpenExternalLinks(true);
    vl->addLayout(hl);
    vl->addWidget(m_image = new QLabel(this));
    m_image->setAlignment(Qt::AlignCenter);
    vl->addWidget(m_body = new QLabel(this));
    m_body->setProperty("role", "body");
    m_body->setWordWrap(true);
    m_body->setOpenExternalLinks(true);
    m_buttonLayout = new QHBoxLayout;
    m_buttonLayout->addStretch();
    m_buttonLayout->addStretch();
    vl->addLayout(m_buttonLayout);
    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    connect (m_timeout, &QTimer::timeout, this, &Notification::timedOut);
}

void Notification::setActions(const QStringList &actions, bool useIcons) {
    while  (m_buttonLayout->count() > 2) {
        QLayoutItem *item = m_buttonLayout->takeAt(1);
        delete item->widget();
        delete item;
    }
    for (int i = 0; i < actions.size(); i+=2) {
        QString label;
        if (i+1 < actions.size())
            label = actions.at(i+1);
        QToolButton *btn = new QToolButton(this);
        btn->setAutoRaise(true);
        btn->setText(label);
        connect(btn, &QToolButton::clicked, [=]() { emit acted(actions.at(i)); });
        if (useIcons)
            btn->setIcon(QIcon::fromTheme(actions.at(i)));
        m_buttonLayout->insertWidget((i/2)+1, btn);
    }
}

void Notification::mouseReleaseEvent(QMouseEvent *event) {
    QWidget::mouseReleaseEvent(event);
    emit ditched();
}

void Notification::setSummary(const QString &summary) {
    m_summaryString = summary;
    m_summary->setText(summary);
    m_summary->setVisible(!summary.isEmpty());
}

void Notification::setBody(const QString &body) {
    m_body->setText(body);
    m_body->setVisible(!body.isEmpty());
}

void Notification::setIcon(const QString &icon) {
    QPixmap pix;
    if (!icon.isEmpty())
        pix = QIcon::fromTheme(icon).pixmap(48);
    m_icon->setPixmap(pix);
    m_icon->setVisible(!pix.isNull());
}

void Notification::setTimeout(int timeout) {
    if (timeout > 0) {
        m_timeout->start(timeout);
        setCountdown(m_countdown);
    }
    else {
        m_timeout->stop();
        setCountdown(false);
    }
}

void Notification::countdown() {
    int remain = m_timeout->remainingTime();
    QString s = m_summaryString;
    if (remain > 60*60*1000) {
        s.replace("%counter%", tr("%1 hours").arg(QString::number(remain/3600000.0f, 'f', 1)));
        int spare = remain/3600000; spare = remain - 3600000*spare;
        m_countdown->start(spare > 10 ? spare : 3600000 + spare);
    } else if (remain > 60*1000) {
        s.replace("%counter%", tr("%1 minutes").arg(qRound(remain/60000.0f)));
        int spare = remain/60000; spare = remain - 60000*spare;
        m_countdown->start(spare > 10 ? spare : 60000 + spare);
    } else {
        s.replace("%counter%", tr("%1 seconds").arg(qRound(remain/1000.0f)));
        int spare = remain/1000; spare = remain - 1000*spare;
        m_countdown->start(spare > 10 ? spare : 1000 + spare);
    }
    m_summary->setText(s);
}

void Notification::setCountdown(bool enabled) {
    if (!enabled) {
        delete m_countdown;
        m_countdown = nullptr;
        return;
    }
    if (!m_countdown) {
        m_countdown = new QTimer(this);
        connect(m_countdown, &QTimer::timeout, this, &Notification::countdown);
    }
    countdown();
}

void Notification::setImage(const QPixmap &pix) {
    m_image->setPixmap(pix);
    m_image->setVisible(!pix.isNull());
}

// ==============================================================

Notifications::Notifications(bool argb) : QFrame() {
    if (argb)
        setAttribute(Qt::WA_TranslucentBackground);
    QDBusConnection::sessionBus().registerService("org.freedesktop.Notifications");
    QDBusConnection::sessionBus().registerObject("/org/freedesktop/Notifications", this);
    new NotiDaptor(this);
//    qEnvironmentVariable(const char *varName, const QString &defaultValue)
    setWindowFlags(Qt::BypassWindowManagerHint);
    setAttribute(Qt::WA_X11NetWmWindowTypeNotification, true);
    m_id = 0;
    m_model = new QStandardItemModel(this);
    QVBoxLayout *vl = new QVBoxLayout(this);
    vl->setContentsMargins(0, 0, 0, 0);
}

/*
Category
"call"	A generic audio or video call notification that doesn't fit into any other category.
"call.ended"	An audio or video call was ended.
"call.incoming"	A audio or video call is incoming.
"call.unanswered"	An incoming audio or video call was not answered.
"device"	A generic device-related notification that doesn't fit into any other category.
"device.added"	A device, such as a USB device, was added to the system.
"device.error"	A device had some kind of error.
"device.removed"	A device, such as a USB device, was removed from the system.
"email"	A generic e-mail-related notification that doesn't fit into any other category.
"email.arrived"	A new e-mail notification.
"email.bounced"	A notification stating that an e-mail has bounced.
"im"	A generic instant message-related notification that doesn't fit into any other category.
"im.error"	An instant message error notification.
"im.received"	A received instant message notification.
"network"	A generic network notification that doesn't fit into any other category.
"network.connected"	A network connection notification, such as successful sign-on to a network service. This should not be confused with device.added for new network devices.
"network.disconnected"	A network disconnected notification. This should not be confused with device.removed for disconnected network devices.
"network.error"	A network-related or connection-related error.
"presence"	A generic presence change notification that doesn't fit into any other category, such as going away or idle.
"presence.offline"	An offline presence change notification.
"presence.online"	An online presence change notification.
"transfer"	A generic file transfer or download notification that doesn't fit into any other category.
"transfer.complete"	A file transfer or download complete notification.
"transfer.error"	A file transfer or download error.
*/

QPixmap Notifications::pixmap(const QDBusArgument &iiibiiay) const
{
    int width, height, rowstride, bitsPerSample, channels;
    bool hasAlpha;
    QByteArray data;

    iiibiiay.beginStructure();
    iiibiiay >> width;
    iiibiiay >> height;
    iiibiiay >> rowstride;
    iiibiiay >> hasAlpha;
    iiibiiay >> bitsPerSample;
    iiibiiay >> channels;
    iiibiiay >> data;
    iiibiiay.endStructure();

    bool rgb = !hasAlpha && channels == 3 && bitsPerSample == 8;
    QImage img = QImage((uchar*)data.constData(), width, height, rgb ? QImage::Format_RGB888 : QImage::Format_ARGB32);
    if (!rgb)
        img = img.rgbSwapped();

    return QPixmap::fromImage(img);
}

QPixmap Notifications::pixmap(const QString &file) const
{
    QPixmap pixmap;
    if (QFile::exists(file)) {
        pixmap = QPixmap(file);
    } else {
        QUrl url(file);
        if (url.isValid() && QFile::exists(url.toLocalFile())) {
            pixmap = QPixmap(url.toLocalFile());
        }
    }
    if (!pixmap.isNull()) {
        // FFS, trotteltech… https://qt-project.atlassian.net/browse/QTBUG-136729
        pixmap.setDevicePixelRatio(qApp->primaryScreen()->devicePixelRatio());
        return pixmap;
    }
    return QIcon::fromTheme(file).pixmap(48);
}

#define HAS_HINT(_H_) (it = hints.constFind(_H_)) != hints.constEnd()
void Notifications::mapHints2Note(const QVariantMap &hints, Notification *note) {
    QVariantMap::const_iterator	it;
    if (HAS_HINT("image-data"))
        note->setImage(pixmap(it->value<QDBusArgument>()));
    else if (HAS_HINT("image_data"))
        note->setImage(pixmap(it->value<QDBusArgument>()));
    else if (HAS_HINT("image_data"))
        note->setImage(pixmap(it->value<QDBusArgument>()));
    else if (HAS_HINT("image-path"))
        note->setImage(pixmap(it->toString()));
    else if (HAS_HINT("image_path"))
        note->setImage(pixmap(it->toString()));
    else if (HAS_HINT("icon_data"))
        note->setImage(pixmap(it->value<QDBusArgument>()));
    else
        note->setImage(QPixmap());

    QString urgency = "normal";
    if (HAS_HINT("urgency")) {
        const int i = it->toInt();
        if (i == 0)
            urgency = "low";
        else if (i == 2)
            urgency = "critical";
    }
    note->setProperty("urgency", urgency);

    if (HAS_HINT("category"))
        note->setProperty("category", it->toString());
}

uint Notifications::add(QString app_name, uint replaces_id, QString app_icon, QString summary, QString body, QStringList actions, QVariantMap hints, int expire_timeout) {
    QStandardItem *item = nullptr;
    if (replaces_id) {
        item = m_idMap.value(replaces_id, nullptr);
    } else {
        while (++m_id < INT_MAX && m_idMap.contains(m_id)) {
            if (m_id == INT_MAX-1)
                m_id = 0; // I don't like the infinite loop but we're also talking about storage limits that need to be addressed anyway
        }
        replaces_id = m_id;
    }
    if (!item) {
        item = new QStandardItem;
        m_model->appendRow(item);
        m_idMap.insert(replaces_id, item);
    }
    item->setText(summary);
    item->setToolTip(body);
    item->setData(app_name, AppName);
    item->setData(app_icon, AppIcon);
    item->setData(actions, Actions);
    item->setData(hints, Hints);
    item->setData(QDateTime::currentSecsSinceEpoch(), Date);
    item->setData(replaces_id, ID);
    Notification *note = item->data(NoteWidget).value<Notification*>();
    if (!note) {
        note = new Notification(this, replaces_id);
        item->setData(QVariant::fromValue(note), NoteWidget);
        connect(note, &Notification::acted, [=](QString action_key) {
            emit acted(note->id(), action_key);
            if (!note->isResident()) {
                close(note->id(), 2); // The notification was dismissed by the user
            }
        });
        connect(note, &Notification::ditched, [=]() {
            close(note->id(), 2); // The notification was dismissed by the user
        });
        connect(note, &Notification::timedOut, [=]() {
            close(note->id(), 1); // The notification expired.
        });
    }
    note->setSummary(summary);
    note->setBody(body);
    note->setIcon(app_icon);
    note->setTimeout(expire_timeout);
    note->setProperty("appname", app_name);
    mapHints2Note(hints, note);

    // these don't make sense on recalls
    QVariantMap::const_iterator	it;
    note->setActions(actions, HAS_HINT("action-icons") && it->toBool());
    note->setResident(HAS_HINT("resident") && it->toBool());
    note->setCountdown(HAS_HINT("countdown") && it->toBool());

    layout()->addWidget(note);
    note->show();
    show();
    adjustGeometry();
    raise();
    return replaces_id;
}

void Notifications::adjustGeometry() {
    adjustSize();
    if (const QScreen *screen = windowHandle()->screen()) {
        QRect r = rect();
        if (m_offset.x() > 0)
            r.moveLeft(screen->geometry().left() + m_offset.x());
        else
            r.moveRight(screen->geometry().right() + m_offset.x());
        if (m_offset.y() > 0)
            r.moveTop(screen->geometry().left() + m_offset.y());
        else
            r.moveBottom(screen->geometry().bottom() + m_offset.y());
        setGeometry(r);
    } else {
        qDebug() << "fuck wayland";
    }
}

void Notifications::close(uint id, int reason) {
    if (QStandardItem *item = m_idMap.value(id, nullptr)) {
        if (Notification *note = item->data(NoteWidget).value<Notification*>()) {
            layout()->removeWidget(note);
            note->deleteLater();
            item->setData(QVariant::fromValue(nullptr), NoteWidget);
            adjustGeometry();
            if (!layout()->count())
                hide();
        }
        const QVariantMap &hints = item->data(Hints).value<QVariantMap>();
        QVariantMap::const_iterator	it;
        if (HAS_HINT("transient") && it->toBool()) {
            QModelIndex dad = item->parent() ? item->parent()->index() : QModelIndex();
            m_model->removeRows(item->row(), 1, dad);
            m_idMap.remove(id);
        }
        if (reason)
            emit closed(id, reason);
    }
}

void Notifications::purge(uint id) {
    close(id, 2);
    if (QStandardItem *item = m_idMap.value(id, nullptr)) {
        QModelIndex dad = item->parent() ? item->parent()->index() : QModelIndex();
        m_model->removeRows(item->row(), 1, dad);
        m_idMap.remove(id);
    }
}

void Notifications::recall(uint id) {
    if (QStandardItem *item = m_idMap.value(id, nullptr)) {
        Notification *note = item->data(NoteWidget).value<Notification*>();
        if (!note) {
            note = new Notification(this, id);
            connect(note, &Notification::ditched, [=]() {
                close(note->id(), 0); // The notification was dismissed by the user, but don't emit a reason
            });
            item->setData(QVariant::fromValue(note), NoteWidget);
            note->setSummary(item->text());
            note->setBody(item->toolTip());
            note->setIcon(item->data(AppIcon).toString());
            // not actions, the user has dismissed the notification and the client might be confused about subsequent actions
            mapHints2Note(item->data(Hints).value<QVariantMap>(), note);
            layout()->addWidget(note);
        }
        note->show();
        show();
        raise();
        adjustGeometry();
    }
}