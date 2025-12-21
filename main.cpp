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

#include <QApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QStyle>
#include <QStyleFactory>
#include <QThread>
#include <QtDBus/QDBusConnectionInterface>
#include <QtDBus/QDBusInterface>
#include <QtDBus/QDBusMessage>

#include <signal.h>
#include <unistd.h>

#include <QtDebug>

#include "qiq.h"

static Qiq *gs_qiq = nullptr;
#ifndef Q_OS_WIN
static void sighandler(int signum) {
    switch(signum) {
//        case SIGABRT:
        case SIGINT:
//        case SIGSEGV:
        case SIGTERM:
            if (gs_qiq) {
                gs_qiq->writeTodoList();
                gs_qiq->writeHistory();
            }
            break;
        default:
            break;
    }
    raise(signum);
}
#endif

void DBusReceptor::ActionInvoked(uint id, QString action_key) {
    if (id == m_id) {
        printf("%s\n", action_key.toLocal8Bit().data());
        quit();
    }
}

void DBusReceptor::NotificationClosed(uint id, uint reason) {
    if (id == m_id) {
        printf("%u\n", reason);
        quit();
    }
}


int notify(const QStringList &args) {
    if (args.isEmpty()) {
        qDebug() << "You need to provide at least a summary";
        return -1;
    }

    QDBusInterface notifications( "org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications" );
    QList<QVariant> vl;
    if (args.at(0).startsWith("close=")) {
        vl << args.at(0).mid(6).toUInt();
        notifications.callWithArgumentList(QDBus::NoBlock, "CloseNotification", vl);
        return 0;
    }

    bool waitForClose = false;
    bool waitForAction = false;
    QString summary = args.at(0);
    QVariantMap hints;
    uint id = 0;
    QString appName, appIcon, body;
    int timeout = 0;
    QStringList actions;

    for (int i = 1; i < args.size(); ++i) {
        if (args.at(i) == "transient")
            hints["transient"] = true;
        else if (args.at(i) == "resident")
            hints["resident"] = true;
        else if (args.at(i) == "wait")
            waitForClose = true;
        else if (args.at(i) == "countdown")
            hints["countdown"] = true;
        else {
            const QString key = args.at(i).section('=',0,0);
            const QString value = args.at(i).section('=',1,-1);
            if (key == "id")
                id = value.toInt();
            else if (key == "body")
                body = value;
            else if (key == "appname")
                appName = value;
            else if (key == "urgency") // low, normal, critical
                hints["urgency"] = value;
            else if (key == "timeout")
                timeout = value.toInt();
            else if (key == "icon")
                appIcon = value;
            else if (key == "image")
                hints["image-path"] = value;
            else if (key == "category")
                hints["category"] = value;
            else if (key == "actions") {
                actions = value.split(',');
                waitForClose = waitForAction = true;
            }
        }
    }
    vl << appName << id << appIcon << summary << body << actions << hints << timeout;
    QDBusReply<uint> reply = notifications.callWithArgumentList(QDBus::Block, "Notify", vl);
    if (reply.isValid()) {
        if (waitForClose) {
            DBusReceptor a(reply.value());
            if (waitForAction)
                QDBusConnection::sessionBus().connect("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                            "org.freedesktop.Notifications", "ActionInvoked", &a, SLOT(ActionInvoked(uint, QString)));
            // unconditionally also to allow handling whatever happens
            QDBusConnection::sessionBus().connect("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                            "org.freedesktop.Notifications", "NotificationClosed", &a, SLOT(NotificationClosed(uint, uint)));
            return a.exec();
        }
        printf("%d\n", reply.value());
        return 0;
    }
    return -1;
}

int main (int argc, char **argv)
{
    QString command;
    QStringList parameters;
    bool isDaemon = false;
    if (argc > 1) {
        QStringList validCommands = QString("daemon\nfilter\nreconfigure\ntoggle\nnotify").split('\n');
        command = QString::fromLocal8Bit(argv[1]);
        if (command == "qiq_daemon") {
            isDaemon = true;
            command = QString();
        }
        if (!isDaemon && !validCommands.contains(command)) {
            qWarning() << "No such command" << command << "\nValid commands:\n" << validCommands;
            return 1;
        }
        for (int i = 2; i < argc; ++i)
            parameters << QString::fromLocal8Bit(argv[i]);
    }
    QDBusConnectionInterface *session = QDBusConnection::sessionBus().interface();
    if (!isDaemon && command.isEmpty() && session->isServiceRegistered("org.qiq.qiq"))
        command = "toggle";
    if (!command.isEmpty()) {
        if (command == "notify")
            return notify(parameters);
        if (!session->isServiceRegistered("org.qiq.qiq")) {
            if (!QProcess::startDetached(argv[0], QStringList() << "qiq_daemon"))
                return 1;
            QElapsedTimer time;
            time.start();
            while (!session->isServiceRegistered("org.qiq.qiq")) {
                if (time.elapsed() > 5000) {
                    qWarning() << "Could not contact nor start daemon, aborting";
                    return 1;
                }
                QThread::msleep(50);
            }
        }
        QDBusInterface qiq( "org.qiq.qiq", "/", "org.qiq.qiq" );
        if (command == "toggle") {
            qiq.call(QDBus::NoBlock, "toggle");
            return 0;
        }
        if (command == "reconfigure") {
            qiq.call(QDBus::NoBlock, "reconfigure");
            return 0;
        }
        if (command == "filter") {
            QList<QVariant> vl;
            for (const QString &s : parameters)
                vl << s;
            if (parameters.size() > 1 && parameters.at(1).startsWith("%print")) {
                QDBusReply<QString> reply = qiq.callWithArgumentList(QDBus::Block, "filter", vl);
                if (reply.isValid())
                    printf("%s\n", reply.value().toLocal8Bit().data());
            } else {
                qiq.callWithArgumentList(QDBus::NoBlock, "filter", vl);
            }
            return 0;
        }
        return 0;
    }

    QStyle *style = QStyleFactory::create("Fusion"); // take some cotrol to allow predictable style sheets
    QApplication::setStyle(style);
    QApplication a(argc, argv);
    QApplication::setStyle(style);
    QCoreApplication::setAttribute(Qt::AA_UseStyleSheetPropagationInWidgetStyles, true);
    Qiq *q = new Qiq;
    if (isDaemon)
        q->hide();
#ifndef Q_OS_WIN
    gs_qiq = q;
    struct sigaction sa;
    sa.sa_handler = sighandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART|SA_RESETHAND;
    for (int signum : {/* SIGABRT,SIGSEGV, */SIGINT,SIGTERM}) {
        if (sigaction(signum, &sa, NULL) == -1)
            qDebug() << "no signal handling for" << signum;
    }
#endif
    return a.exec();
}