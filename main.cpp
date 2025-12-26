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
#include <QRegularExpression>
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
static char *gs_appname;
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

void helpNotify() {
    printf("Usage: %s notify <summary> [<features>]\n"
           "       %s notify close <ID>\n", gs_appname, gs_appname);
    printf(R"(
Sending a non-waiting notification prints the returned ID of that notification

Features can be:
    actions=<action,Label[,…]>
            This implicitly waits and prints the invoked action or other closing reason
    appname=<appname, also serves as style indictor>
       body=<longer text>
   category=<category that can be used as style indicator>
countdown  |the summary token "%%counter%%" will be replaced by the remaining timeout
       icon=<app icon name or full path>
         id=<notification id to replace>
      image=<path to an image to show>
resident   |the notification isn't closed by using any action
transient  |the notification isn't logged
    urgency=<low|normal|critical>
wait       |wait until the notification closes and print the reason
            0: undefined, 1: expired, 2: dismissed, 3: closed by a call to CloseNotification
Example:
--------
%s notify SNAFU "body=foo bar baz" appname=snafu actions=abort,Abort,cancel,Cancel
)", gs_appname);
}

void help() {
    printf(
"Usage:     %s ask <question> [<echo mode>]\n"
"           %s countdown <timeout> [<message>]\n"
"           %s daemon\n"
"           %s filter <file> [<action> [<field separator>]]\n"
"           %s notify <summary> [<features>]\n"
"           %s reconfigure\n"
"           %s toggle\n", gs_appname, gs_appname, gs_appname, gs_appname, gs_appname, gs_appname, gs_appname);
    printf(R"(-------------------------------------------------------------------------------------------------------------------
ask         Ask the user to enter some test that will be printed to stdout
            The echo mode can be "normal" (default) or "password"
countdown   Run a countdown notification with optional message.
            Either fragment of h:m.s or XhYmZs will work (5.30 or 5m30s)
            A single number without any suffix is accepted as milliseconds.

daemon      Explicitly fork a daemon process, immediately exits

filter      filter <file> [<action> [<field separator>]
            Allow the use to filter through the lines of a file and pass the accepted line to an action

            The special actions "%%clip" and "%%print" will put the result on the clipboard or (wait and) print it to stdout
            They also allow to remove or replace regular expressions from the result, eg. '%%clip/^[^\|]*\| //%%CRLF%%/\n'
            will remove anything before the first "| " and replace "%%CRLF%%" with "\n" (not! a newline)
            like with the sed "s" operator the first char becomes the instruction separator, this does not have to be
            the slash "/". Eg. '%%print%%secret' will just remove every occurrence of "secret"
            Keep in mind that the serach tokens need to be escaped for regular expressions

            The field separator is an arbitrary string that allows to show human readable text (the first field)
            but pass a number or other technical value to the action.

notify      send a https://xdg.pages.freedesktop.org/xdg-specs/notification
            prints long help when invoked without any parameter

reconfigure reload the configuration and update Qiq

toggle      shows, hides or activates Qiq depending on its current state
            It's what you want to bind your shortcut to ;)
)");
}

int notify(const QStringList &args) {
    if (args.isEmpty()) {
        helpNotify();
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
            else if (key == "urgency") { // low, normal, critical
                if (value == "critical")
                    hints["urgency"] = 2;
                else if  (value == "low")
                    hints["urgency"] = 0;
                else
                    hints["urgency"] = 1;
            }
            else if (key == "timeout")
                timeout = Qiq::msFromString(value);
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
    gs_appname = argv[0];
    QString command;
    QStringList parameters;
    bool isDaemon = false;
    if (argc > 1) {
        QStringList validCommands = QString("ask\ncountdown\ndaemon\nfilter\nreconfigure\ntoggle\nnotify").split('\n');
        command = QString::fromLocal8Bit(argv[1]);
        if (command == "qiq_daemon") {
            isDaemon = true;
            command = QString();
        }
        if (!isDaemon && !validCommands.contains(command)) {
            help();
            return 1;
        }
        for (int i = 2; i < argc; ++i)
            parameters << QString::fromLocal8Bit(argv[i]);
    }
    QDBusConnectionInterface *session = QDBusConnection::sessionBus().interface();
    if (!isDaemon && command.isEmpty() && session->isServiceRegistered("org.qiq.qiq"))
        command = "toggle";
    if (!command.isEmpty()) {
        if (command == "countdown") {
            command = "notify";
            if (parameters.count() < 1) {
                printf("%s countdown <time> [<message>]\n", gs_appname);
                return 1;
            }
            const QString timeout = parameters.takeLast();
            int ms = Qiq::msFromString(timeout);
            if (ms < 0) {
                qDebug() << "invalid timeout" << timeout;
                return 1;
            }

            QString summary = parameters.join(" ");
            if (!summary.contains("%counter%"))
                summary.append(" %counter%");
            parameters.clear();
            parameters << summary << "timeout=" + QString::number(ms) << "transient" << "countdown";
        }
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
        if (command == "ask") {
            if (parameters.count() < 1) {
                printf("%s ask <question> [<echo mode>]\n", gs_appname);
                return 1;
            }
            QList<QVariant> vl;
            for (const QString &s : parameters)
                vl << s;
            QDBusReply<QString> reply = qiq.callWithArgumentList(QDBus::Block, "ask", vl);
            if (reply.isValid()) {
                printf("%s\n", reply.value().toLocal8Bit().data());
                return 0;
            }
            return 1;
        }
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