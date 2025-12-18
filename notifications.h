#ifndef NOTIFICATIONS_H
#define NOTIFICATIONS_H

#include <QtDBus/QDBusAbstractAdaptor>
#include <QtDBus/QDBusArgument>
#include <QFrame>
#include <QLabel>

class QHBoxLayout;
class QStandardItem;
class QStandardItemModel;
class QTimer;

class Notification : public QFrame {
    Q_OBJECT
public:
    Notification(QWidget *parent, uint id);
    uint id() const { return m_id; }
    bool isResident() const { return m_resident; }
    void setActions(const QStringList &actions, bool useIcons);
    void setBody(const QString &body);
    void setCountdown(bool countdown);
    void setIcon(const QString &icon);
    void setImage(const QPixmap &pix);
    void setResident(bool resident) { m_resident = resident; }
    void setSummary(const QString &summary);
    void setTimeout(int timeout);
signals:
    void acted(QString action_key);
    void ditched();
    void timedOut();
protected:
    void mouseReleaseEvent(QMouseEvent *event);
private:
    void countdown();
    QLabel *m_icon, *m_summary, *m_image, *m_body;
    QHBoxLayout *m_buttonLayout;
    uint m_id;
    bool m_resident;
    QTimer *m_timeout, *m_countdown;
    QString m_summaryString;
};


class Notifications : public QFrame {
    Q_OBJECT
public:
    Notifications();
    enum NotStuff { AppName = Qt::UserRole + 1, AppIcon, Body, Actions, Hints, Date, NoteWidget };
    uint add(QString app_name, uint replaces_id, QString app_icon, QString summary, QString body, QStringList actions, QVariantMap hints, int expire_timeout);
    void adjustGeometry();
    void close(uint id, int reason);
    QStandardItemModel *model() { return m_model; }
    QPixmap pixmap(const QDBusArgument &iiibiiay) const;
    QPixmap pixmap(const QString &file) const;
    void purge(uint id);
signals:
    void acted(uint id, QString action_key);
    void closed(uint id, uint reason);
private:
    uint m_id;
    QStandardItemModel *m_model;
    QMap<int, QStandardItem*> m_idMap;
};


// https://xdg.pages.freedesktop.org/xdg-specs/notification/latest-single/
class NotiDaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    // org.freedesktop.Notifications "/org/freedesktop/Notifications"
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
public:
    NotiDaptor(Notifications *n) : QDBusAbstractAdaptor(n), notifications(n) {
        connect(n, SIGNAL(acted(uint, QString)), SIGNAL(ActionInvoked(uint, QString)));
        connect(n, SIGNAL(closed(uint, uint)), SIGNAL(NotificationClosed(uint, uint)));
    }
public slots:
    void CloseNotification(uint id) {
        notifications->close(id, 3); // The notification was closed by a call to CloseNotification.
    }
    QStringList GetCapabilities() {
        return QString(/* "action-icons, */"actions,body,body-images,body-hyperlinks,body-markup,icon-multi,icon-static,persistence").split(',');
//        ",synchronous,private-synchronous,x-canonical-private-synchronous,x-dunst-stack-tag")
    }
    void GetServerInformation(QString& name, QString& vendor, QString& version, QString& spec_version) {
        name = "qiq"; vendor = "qiq"; version = "0.1"; spec_version = "1.3";
    }
    uint Notify(QString app_name, uint replaces_id, QString app_icon, QString summary, QString body, QStringList actions, QVariantMap hints, int expire_timeout) {
        return notifications->add(app_name, replaces_id, app_icon, summary, body, actions, hints, expire_timeout);
    }
signals:
    void ActionInvoked(uint id, QString action_key);
    void NotificationClosed(uint id, uint reason);
private:
    Notifications *notifications;
};

#endif // NOTIFICATIONS_H