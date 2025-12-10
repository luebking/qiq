#ifndef QIQ_H
#define QIQ_H

#include <QtDBus/QDBusAbstractAdaptor>
#include <QStackedWidget>

class QFileSystemModel;
class QStandardItemModel;
class QStringListModel;
class QListView;
class QTextBrowser;
class QLineEdit;

class Qiq : public QStackedWidget {
    Q_OBJECT
public:
    Qiq();
    QString filterCustom(const QString source, const QString action = QString(), const QString fieldSeparator = QString());
    void reconfigure();
    void toggle();
protected:
    void enterEvent(QEnterEvent *ee) override;
    bool eventFilter(QObject *o, QEvent *e) override;
private:
    enum MatchType { Begin = 0, Partial };
    enum AppStuff { AppExec = Qt::UserRole + 1, AppComment, AppPath, AppNeedsTE, AppCategories, AppKeywords };
    void adjustGeometry();
    void explicitlyComplete(const QString token);
    void filter(const QString needle, MatchType matchType);
    void filterInput();
    void insertToken();
    void makeApplicationModel();
    void printOutput(int exitCode);
    bool runInput();
    QListView *m_list;
    QTextBrowser *m_disp;
    QLineEdit *m_input;
    QWidget *m_status;
    QStandardItemModel *m_applications, *m_external;
    QStringListModel *m_bins;
    QFileSystemModel *m_files;
    QSize m_defaultSize;
    int m_lastVisibleRow;
    QString m_externCmd, m_externalReply;
    bool m_wasVisble;
    QHash<QString,QString> m_aliases;
    QString m_aha, m_qalc;
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

#endif // QIQ_H