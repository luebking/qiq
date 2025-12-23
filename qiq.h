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

#ifndef QIQ_H
#define QIQ_H

#include <QCoreApplication>
#include <QtDBus/QDBusAbstractAdaptor>
#include <QStackedWidget>
#include <QTimer>

class Notifications;
class QAbstractItemModel;
class QFileSystemModel;
class QStandardItemModel;
class QStringListModel;
class QListView;
class QTextBrowser;
class QTextEdit;
class QLineEdit;

class Qiq : public QStackedWidget {
    Q_OBJECT
public:
    Qiq();
    QString filterCustom(const QString source, const QString action = QString(), const QString fieldSeparator = QString());
    void reconfigure();
    static int msFromString(const QString &string);
    void toggle();
    void writeHistory();
    void writeTodoList();
protected:
    void enterEvent(QEnterEvent *ee) override;
    bool eventFilter(QObject *o, QEvent *e) override;
private:
    enum MatchType { Begin = 0, Partial };
    enum AppStuff { AppExec = Qt::UserRole + 1, AppComment, AppPath, AppNeedsTE, AppCategories, AppKeywords, MatchScore };
    void adjustGeometry();
    void explicitlyComplete();
    void filter(const QString needle, MatchType matchType);
    void filterInput();
    void insertToken();
    void makeApplicationModel();
    void message(const QString &string);
    void notifyUser(const QString &summary, const QString &body, int urgency = 1);
    void printOutput(int exitCode);
    bool runInput();
    void setModel(QAbstractItemModel *model);
    void tokenUnderCursor(int &left, int &right);
    void updateTodoTimers();
    QListView *m_list;
    QTextBrowser *m_disp;
    QLineEdit *m_input;
    QWidget *m_status;
    QStandardItemModel *m_applications, *m_external;
    QStringListModel *m_bins, *m_cmdHistory, *m_cmdCompleted;
    QFileSystemModel *m_files;
    QSize m_defaultSize;
    int m_lastVisibleRow;
    QString m_externCmd, m_externalReply;
    bool m_wasVisble;
    QHash<QString,QString> m_aliases;
    QString m_aha, m_qalc, m_term, m_cmdCompletion, m_cmdCompletionSep;
    QStringList m_history;
    int m_currentHistoryIndex;
    QString m_inputBuffer;
    QTimer m_autoHide;
    QTimer *m_historySaver;
    QString m_historyPath;
    Notifications *m_notifications;
    QTextEdit *m_todo;
    QList<QTimer*> m_todoTimers;
    bool m_todoDirty, m_todoSaved;
    QString m_todoPath;
    QTimer *m_todoSaver;
    int m_iconSize;
};

class DBusAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.qiq.qiq")
public:
    DBusAdaptor(Qiq *q) : QDBusAbstractAdaptor(q), qiq(q) { }
public slots:
    QString filter(const QString source, const QString action = QString(), const QString fieldSeparator = QString()) {
        return qiq->filterCustom(source, action, fieldSeparator);
    }
    Q_NOREPLY void toggle() { qiq->toggle(); }
    Q_NOREPLY void reconfigure() { qiq->reconfigure(); }
private:
    Qiq *qiq;
};

class DBusReceptor : public QCoreApplication
{
    Q_OBJECT
public:
    DBusReceptor(uint id, int argc = 0, char **argv = nullptr) : QCoreApplication(argc, argv), m_id(id) { }
public slots:
    void ActionInvoked(uint id, QString action_key);
    void NotificationClosed(uint id, uint reason);
private:
    uint m_id;
};

#endif // QIQ_H